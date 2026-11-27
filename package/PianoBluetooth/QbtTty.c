/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* macOS front end of the transport: the serial device /dev/cu.BLTH.
 *
 * macOS bluetoothd reaches an H4 controller through a BSD serial device (its "POSIX" transport: open, TIOCEXCL, tcgetattr,
 * cfmakeraw, tcsetattr, IOSSIOSPEED, then select + read on one thread and write on another). iPadOS uses the UART.SK kernel
 * control and a Skywalk pipe instead (qbt_transport.c). Both front ends implement qbt_transport.h over the same two byte FIFOs;
 * the UART, its speed and the controller's bring-up stay with the board's worker, so the line settings a client sets here are
 * recorded and answered, not applied. */
#include "qbt_transport.h"
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/conf.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/kauth.h>
#include <sys/proc.h>
#include <sys/select.h>
#include <sys/sysctl.h>
#include <sys/uio.h>
#include <sys/vnode.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <miscfs/devfs/devfs.h>
#include "BtUartRing.h"
extern kern_return_t thread_terminate(thread_t);
#define BT_TTY_NAME "cu.BLTH"
#define BT_LOG(fmt, ...) do { \
    if(qbt_bt_trace_enabled)printf(fmt " [uptime_ms=%llu]\n", ##__VA_ARGS__, qbt_bt_uptime_ms()); \
    else printf(fmt "\n", ##__VA_ARGS__); \
} while(0)
/* The ioctls of a 64-bit client (sys/ttycom.h, sys/filio.h, IOKit/serial/ioss.h). */
#define BT_TIOCGETA   0x40487413u
#define BT_TIOCSETA   0x80487414u
#define BT_TIOCSETAW  0x80487415u
#define BT_TIOCSETAF  0x80487416u
#define BT_TIOCEXCL   0x2000740du
#define BT_TIOCNXCL   0x2000740eu
#define BT_TIOCFLUSH  0x80047410u
#define BT_TIOCDRAIN  0x2000745eu
#define BT_TIOCOUTQ   0x40047473u
#define BT_TIOCMGET   0x4004746au
#define BT_TIOCMSET   0x8004746du
#define BT_TIOCMBIS   0x8004746cu
#define BT_TIOCMBIC   0x8004746bu
#define BT_TIOCSDTR   0x20007479u
#define BT_TIOCCDTR   0x20007478u
#define BT_FIONREAD   0x4004667fu
#define BT_FIONBIO    0x8004667eu
#define BT_FIOASYNC   0x8004667du
#define BT_IOSSIOSPEED 0x80085402u
struct bt_termios64 {
    uint64_t iflag,oflag,cflag,lflag;
    uint8_t cc[20];
    uint32_t pad;
    uint64_t ispeed,ospeed;
};
_Static_assert(sizeof(struct bt_termios64) == 72, "27.3 termios64 ABI");
static struct qbt_transport_ops bt_ops;
/* Kernel STARTUP records are not run for this kext. Initialize before any
 * sysctl or cdev can expose the transport, and retain the lock while its
 * worker and device callbacks exist (stop only unregisters the sysctls). */
static lck_grp_t *bt_tty_grp;
static lck_mtx_t bt_tty_lock;
static unsigned bt_tty_init_claimed;
static struct {
    int major;
    void *node;
    bool open,closing,node_thread;
    bool rsel_used,wsel_used;
    struct selinfo rsel,wsel;
    struct bt_termios64 termios;
    uint32_t speed;
    uint32_t rx_head,rx_len,tx_head,tx_len;
    uint8_t rx[BT_TTY_FIFO],tx[BT_TTY_FIFO];
    int boot;           /* -1 pending, 0 ready, else errno */
    bool power_pending;
    int power_result;
    char firmware[64];
    uint64_t rx_dropped;
} bt_tty={.major=-1,.boot=-1,.speed=115200};

/* Native stack activity: power requests, boot-state reads, opens, speed requests. */
volatile uint32_t qbt_transport_activity[4];
static inline void bt_tty_count(unsigned index)
{
    (void)__atomic_add_fetch(&qbt_transport_activity[index],1u,__ATOMIC_RELAXED);
}
static void bt_kick(void)
{
    if(bt_ops.wake)bt_ops.wake(bt_ops.context);
}
static void bt_tty_termios_default(void)
{
    memset(&bt_tty.termios,0,sizeof(bt_tty.termios));
    /* The hardware line: 8N1, CREAD, RTS/CTS. */
    bt_tty.termios.cflag=0x300|0x800|0x8000|0x10000|0x20000;
    bt_tty.termios.ispeed=bt_tty.termios.ospeed=bt_tty.speed;
}
/* Called with the lock held: readers, then select and kqueue. */
static void bt_tty_wake_readers(void)
{
    wakeup(&bt_tty.rx_len);
    if(bt_tty.rsel_used){bt_tty.rsel_used=false;selwakeup(&bt_tty.rsel);}
}
static void bt_tty_wake_writers(void)
{
    wakeup(&bt_tty.tx_len);
    if(bt_tty.wsel_used){bt_tty.wsel_used=false;selwakeup(&bt_tty.wsel);}
}

/* UART owner side. receive returns the bytes taken: all of them while nobody has the device open (discarded), else what
 * fits in the FIFO; the owner keeps the rest and offers it again. */
uint32_t qbt_transport_receive(const uint8_t *data,uint32_t n)
{
    if(!n) return 0;
    lck_mtx_lock(&bt_tty_lock);
    if(!bt_tty.open || bt_tty.closing || bt_tty.boot) {
        bt_tty.rx_dropped+=n;lck_mtx_unlock(&bt_tty_lock);return n;
    }
    if(n>BT_TTY_FIFO-bt_tty.rx_len) n=BT_TTY_FIFO-bt_tty.rx_len;
    if(n) {
        bt_tty_fifo_put(bt_tty.rx,bt_tty.rx_head,&bt_tty.rx_len,data,n);
        bt_tty_wake_readers();
    }
    lck_mtx_unlock(&bt_tty_lock);
    return n;
}
uint32_t qbt_transport_tx_pending(void)
{
    lck_mtx_lock(&bt_tty_lock);
    uint32_t n=bt_tty.tx_len;
    lck_mtx_unlock(&bt_tty_lock);
    return n;
}
uint32_t qbt_transport_tx_take(uint8_t *data,uint32_t max)
{
    lck_mtx_lock(&bt_tty_lock);
    uint32_t n=bt_tty.tx_len<max?bt_tty.tx_len:max;
    bt_tty_fifo_get(bt_tty.tx,&bt_tty.tx_head,&bt_tty.tx_len,data,n);
    if(n) bt_tty_wake_writers();
    lck_mtx_unlock(&bt_tty_lock);
    return n;
}
bool qbt_transport_active(void)
{
    lck_mtx_lock(&bt_tty_lock);
    bool active=bt_tty.open && !bt_tty.closing && !bt_tty.boot;
    lck_mtx_unlock(&bt_tty_lock);return active;
}

/* Asks the worker to start the controller and waits for its result (the worker keeps a running controller as it is).
 * Called without the lock; returns 0 or an errno. */
static int bt_tty_power_on(void)
{
    int error=0,result;
    lck_mtx_lock(&bt_tty_lock);
    if(bt_tty.power_pending || !bt_ops.power){lck_mtx_unlock(&bt_tty_lock);return EBUSY;}
    bt_tty.power_pending=true;bt_tty.power_result=EINPROGRESS;
    lck_mtx_unlock(&bt_tty_lock);
    error=bt_ops.power(bt_ops.context,true);
    lck_mtx_lock(&bt_tty_lock);
    if(error){bt_tty.power_pending=false;bt_tty.power_result=error;}
    struct timespec timeout={30,0};
    while(bt_tty.power_pending && !error)error=msleep(&bt_tty.power_pending,&bt_tty_lock,PZERO|PCATCH,"btpower",&timeout);
    result=bt_tty.power_result;lck_mtx_unlock(&bt_tty_lock);
    return error?error:result;
}

/* The serial device. One client at a time, as the daemon asks with TIOCEXCL. */
static int bt_tty_open(dev_t dev __unused,int flags __unused,int devtype __unused,struct proc *p)
{
    if(qbt_bt_trace_enabled)BT_LOG("[bluetooth tty] open begin pid=%d",proc_pid(p));
    lck_mtx_lock(&bt_tty_lock);
    if(bt_tty.open){
        lck_mtx_unlock(&bt_tty_lock);
        if(qbt_bt_trace_enabled)BT_LOG("[bluetooth tty] open end error=%d",EBUSY);
        return EBUSY;
    }
    bt_tty.open=true;bt_tty.closing=false;
    bt_tty.rx_head=bt_tty.rx_len=bt_tty.tx_head=bt_tty.tx_len=0;
    bt_tty_termios_default();
    int boot=bt_tty.boot;
    /* The worker's first bring-up may still be running: wait for its result before deciding. */
    struct timespec timeout={30,0};
    int error=0;
    while(bt_tty.boot<0 && !error)error=msleep(&bt_tty.boot,&bt_tty_lock,PZERO|PCATCH,"btopen",&timeout);
    boot=bt_tty.boot;
    lck_mtx_unlock(&bt_tty_lock);
    bt_tty_count(2);
    /* A client on a stopped controller restarts it. */
    if(!error && boot>0)error=bt_tty_power_on();
    BT_LOG("[bluetooth tty] open pid=%d boot=%d error=%d",proc_pid(p),boot,error);
    if(error) {
        lck_mtx_lock(&bt_tty_lock);bt_tty.open=false;lck_mtx_unlock(&bt_tty_lock);
        if(error==EWOULDBLOCK)error=EIO;
    }
    else bt_kick();
    if(qbt_bt_trace_enabled)BT_LOG("[bluetooth tty] open end error=%d",error);
    return error;
}
static int bt_tty_close(dev_t dev __unused,int flags __unused,int devtype __unused,struct proc *p __unused)
{
    lck_mtx_lock(&bt_tty_lock);
    bt_tty.closing=true;
    bt_tty_wake_readers();bt_tty_wake_writers();
    BT_LOG("[bluetooth tty] close rx_dropped=%llu",bt_tty.rx_dropped);
    bt_tty.open=bt_tty.closing=false;
    bt_tty.rx_head=bt_tty.rx_len=bt_tty.tx_head=bt_tty.tx_len=0;
    lck_mtx_unlock(&bt_tty_lock);
    bt_kick();
    return 0;
}
/* Called with the lock held. 0 while I/O may go on, EAGAIN while the controller starts. */
static int bt_tty_io_status(void)
{
    if(!bt_tty.open || bt_tty.closing) return ENXIO;
    return bt_tty.boot<0?EAGAIN:bt_tty.boot?EIO:0;
}
static int bt_tty_read(dev_t dev __unused,struct uio *uio,int ioflag)
{
    uint8_t chunk[512];
    int error=0;
    lck_mtx_lock(&bt_tty_lock);
    while(!bt_tty.rx_len) {
        error=bt_tty_io_status();
        if(error && error!=EAGAIN) break;
        if(ioflag&IO_NDELAY){error=EAGAIN;break;}
        error=msleep(&bt_tty.rx_len,&bt_tty_lock,PZERO|PCATCH,"btread",NULL);
        if(error) break;
    }
    if(error){lck_mtx_unlock(&bt_tty_lock);return error;}
    bool taken=false;
    while(bt_tty.rx_len && uio_resid(uio)>0) {
        uint32_t n=(uint32_t)MIN((user_ssize_t)MIN(bt_tty.rx_len,sizeof(chunk)),uio_resid(uio));
        bt_tty_fifo_get(bt_tty.rx,&bt_tty.rx_head,&bt_tty.rx_len,chunk,n);
        taken=true;
        lck_mtx_unlock(&bt_tty_lock);
        error=uiomove((const char *)chunk,(int)n,uio);
        lck_mtx_lock(&bt_tty_lock);
        if(error) break;
    }
    lck_mtx_unlock(&bt_tty_lock);
    /* Room in the FIFO: the owner offers what it held back. */
    if(taken) bt_kick();
    return error;
}
static int bt_tty_write(dev_t dev __unused,struct uio *uio,int ioflag)
{
    uint8_t chunk[512];
    int error=0;
    while(uio_resid(uio)>0 && !error) {
        uint32_t n=(uint32_t)MIN((user_ssize_t)sizeof(chunk),uio_resid(uio));
        lck_mtx_lock(&bt_tty_lock);
        /* Whole chunks only, so a failed wait leaves the client's data untouched. */
        while(!(error=bt_tty_io_status()) && n>BT_TTY_FIFO-bt_tty.tx_len) {
            if(ioflag&IO_NDELAY){error=EAGAIN;break;}
            error=msleep(&bt_tty.tx_len,&bt_tty_lock,PZERO|PCATCH,"btwrite",NULL);
            if(error) break;
        }
        lck_mtx_unlock(&bt_tty_lock);
        if(error) break;
        error=uiomove((char *)chunk,(int)n,uio);
        if(error) break;
        lck_mtx_lock(&bt_tty_lock);
        error=bt_tty_io_status();
        /* This device has one writer (the daemon's HCI thread); another writer that took the room is reported, not interleaved. */
        if(!error && n>BT_TTY_FIFO-bt_tty.tx_len) error=EIO;
        if(!error) bt_tty_fifo_put(bt_tty.tx,bt_tty.tx_head,&bt_tty.tx_len,chunk,n);
        lck_mtx_unlock(&bt_tty_lock);
        if(!error) bt_kick();
    }
    return error;
}
static int bt_tty_select(dev_t dev __unused,int which,void *wql,struct proc *p)
{
    int ready=0;
    lck_mtx_lock(&bt_tty_lock);
    int status=bt_tty_io_status();
    if(which==FREAD) {
        if(bt_tty.rx_len || (status && status!=EAGAIN)) ready=1;
        else {bt_tty.rsel_used=true;selrecord(p,&bt_tty.rsel,wql);}
    } else if(which==FWRITE) {
        if(bt_tty.tx_len<BT_TTY_FIFO || (status && status!=EAGAIN)) ready=1;
        else {bt_tty.wsel_used=true;selrecord(p,&bt_tty.wsel,wql);}
    }
    lck_mtx_unlock(&bt_tty_lock);
    return ready;
}
static int bt_tty_ioctl(dev_t dev __unused,u_long cmd,caddr_t data,int fflag __unused,struct proc *p __unused)
{
    int error=0;
    lck_mtx_lock(&bt_tty_lock);
    switch((uint32_t)cmd) {
    case BT_TIOCGETA:
        memcpy(data,&bt_tty.termios,sizeof(bt_tty.termios));break;
    case BT_TIOCSETA: case BT_TIOCSETAW: case BT_TIOCSETAF:
        /* Recorded for the next TIOCGETA. The line is fixed at 8N1 with RTS/CTS at the speed of the bring-up. */
        memcpy(&bt_tty.termios,data,sizeof(bt_tty.termios));break;
    case BT_IOSSIOSPEED: {
        uint64_t speed;memcpy(&speed,data,sizeof(speed));
        bt_tty.termios.ispeed=bt_tty.termios.ospeed=speed;
        bt_tty_count(3);
        BT_LOG("[bluetooth tty] speed request %llu, line %u",speed,bt_tty.speed);
        break;
    }
    case BT_TIOCFLUSH: {
        int what;memcpy(&what,data,sizeof(what));
        if(!what || (what&FREAD)) bt_tty.rx_head=bt_tty.rx_len=0;
        break;
    }
    case BT_TIOCOUTQ: {int n=(int)bt_tty.tx_len;memcpy(data,&n,sizeof(n));break;}
    case BT_FIONREAD: {int n=(int)bt_tty.rx_len;memcpy(data,&n,sizeof(n));break;}
    case BT_TIOCMGET: {int lines=0x002|0x004|0x020|0x040|0x100;memcpy(data,&lines,sizeof(lines));break;}  /* DTR RTS CTS CAR DSR */
    case BT_TIOCDRAIN: {
        struct timespec timeout={1,0};
        while(bt_tty.tx_len && !error && !bt_tty_io_status())error=msleep(&bt_tty.tx_len,&bt_tty_lock,PZERO|PCATCH,"btdrain",&timeout);
        if(error==EWOULDBLOCK) error=0;
        break;
    }
    case BT_TIOCEXCL: case BT_TIOCNXCL: case BT_TIOCMSET: case BT_TIOCMBIS: case BT_TIOCMBIC: case BT_TIOCSDTR: case BT_TIOCCDTR:
    case BT_FIONBIO: case BT_FIOASYNC:
        break;
    default:
        error=ENOTTY;
    }
    lck_mtx_unlock(&bt_tty_lock);
    return error;
}
static const struct cdevsw bt_tty_cdevsw={
    .d_open=bt_tty_open,.d_close=bt_tty_close,.d_read=bt_tty_read,.d_write=bt_tty_write,.d_ioctl=bt_tty_ioctl,
    .d_stop=eno_stop,.d_reset=eno_reset,.d_ttys=NULL,.d_select=bt_tty_select,.d_mmap=eno_mmap,.d_strategy=eno_strat,
    .d_reserved_1=eno_getc,.d_reserved_2=eno_putc,.d_type=0,
};
/* devfs takes nodes once it is ready; the board's worker can start before that. */
static bool bt_tty_make_node(void)
{
    lck_mtx_lock(&bt_tty_lock);
    if(bt_tty.major<0) {
        bt_tty.major=cdevsw_add(-1,&bt_tty_cdevsw);
        if(bt_tty.major>=0)(void)cdevsw_setkqueueok(bt_tty.major,&bt_tty_cdevsw,0);
    }
    if(bt_tty.major>=0 && !bt_tty.node)
        bt_tty.node=devfs_make_node(makedev(bt_tty.major,0),DEVFS_CHAR,UID_ROOT,GID_WHEEL,0600,BT_TTY_NAME);
    bool made=bt_tty.node!=NULL;
    lck_mtx_unlock(&bt_tty_lock);
    return made;
}
static void bt_tty_node_thread(void *context __unused,wait_result_t result __unused)
{
    unsigned tries=0;
    while(!bt_tty_make_node() && ++tries<600) {
        struct timespec pause={0,100*1000*1000};
        (void)msleep(&bt_tty.node_thread,NULL,PZERO,"btnode",&pause);
    }
    BT_LOG("[bluetooth tty] /dev/" BT_TTY_NAME " %s after %u waits",bt_tty.node?"made":"not made",tries);
    thread_terminate(current_thread());
}

static int bt_tty_boot_sysctl SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp,arg1,arg2)
    int error=0,value;
    bt_tty_count(1);
    lck_mtx_lock(&bt_tty_lock);
    struct timespec timeout={30,0};
    while(bt_tty.boot<0 && !error) error=msleep(&bt_tty.boot,&bt_tty_lock,PZERO|PCATCH,"btboot",&timeout);
    value=bt_tty.boot;
    lck_mtx_unlock(&bt_tty_lock);
    return error?error:SYSCTL_OUT(req,&value,sizeof(value));
}

/* A sandboxed client cannot write sysctls, so reading this value requests startup and waits for the worker's result. */
static int bt_tty_power_on_sysctl SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp,arg1,arg2)
    int result=0;
    if(req->newptr)return EPERM;
    if(!req->oldptr)return SYSCTL_OUT(req,&result,sizeof(result));
    bt_tty_count(0);
    if(qbt_bt_trace_enabled)BT_LOG("[bluetooth tty] BlueTool power request begin");
    result=bt_tty_power_on();
    if(qbt_bt_trace_enabled)BT_LOG("[bluetooth tty] BlueTool power request end error=%d",result);
    return SYSCTL_OUT(req,&result,sizeof(result));
}
static int bt_uptime_sysctl SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp,arg1,arg2)
    if(req->newptr)return EPERM;
    uint64_t uptime=qbt_bt_uptime_ms();
    return SYSCTL_OUT(req,&uptime,sizeof(uptime));
}

static int bt_baseline_released;

void qbt_transport_wait_for_baseline(void)
{
    lck_mtx_lock(&bt_tty_lock);
    while (!bt_baseline_released)
        (void)msleep(&bt_baseline_released, &bt_tty_lock, PZERO, "btbaseline", NULL);
    lck_mtx_unlock(&bt_tty_lock);
}

static int bt_baseline_sysctl SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp,arg1,arg2)
    if (!kauth_cred_issuser(kauth_cred_get())) return EPERM;
    lck_mtx_lock(&bt_tty_lock);
    const int released = bt_baseline_released;
    lck_mtx_unlock(&bt_tty_lock);
    int error = SYSCTL_OUT(req, &released, sizeof released);
    if (error || !req->newptr) return error;
    int requested = 0;
    error = SYSCTL_IN(req, &requested, sizeof requested);
    if (error) return error;
    if (requested != 1) return EINVAL;
    lck_mtx_lock(&bt_tty_lock);
    bt_baseline_released = 1;
    wakeup(&bt_baseline_released);
    lck_mtx_unlock(&bt_tty_lock);
    return 0;
}

/* piano-bt-lemask at run time: reads the current value; root writes 0 or 1. bluetoothd sends its LE event mask at HCI start, so a
 * change takes effect at the daemon's next start (or a radio off/on). */
static int bt_lemask_sysctl SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp,arg1,arg2)
    int value = (int)qbt_bt_lemask;
    int error = SYSCTL_OUT(req, &value, sizeof value);
    if (error || !req->newptr) return error;
    if (!kauth_cred_issuser(kauth_cred_get())) return EPERM;
    int requested = 0;
    error = SYSCTL_IN(req, &requested, sizeof requested);
    if (error) return error;
    if (requested != 0 && requested != 1) return EINVAL;
    qbt_bt_lemask = (uint32_t)requested;
    BT_LOG("PianoBluetooth: bt-lemask1: machdep.piano_bt_lemask=%d", requested);
    return 0;
}

/* piano-bt-lerdy at run time (HciH4.h LeReadyAssist): reads the current value; root writes 0 or 1. Applies to the assist's next
 * step for every LE link it tracks. */
static int bt_lerdy_sysctl SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp,arg1,arg2)
    int value = (int)qbt_bt_lerdy;
    int error = SYSCTL_OUT(req, &value, sizeof value);
    if (error || !req->newptr) return error;
    if (!kauth_cred_issuser(kauth_cred_get())) return EPERM;
    int requested = 0;
    error = SYSCTL_IN(req, &requested, sizeof requested);
    if (error) return error;
    if (requested != 0 && requested != 1) return EINVAL;
    qbt_bt_lerdy = (uint32_t)requested;
    BT_LOG("PianoBluetooth: bt-lerdy1: machdep.piano_bt_lerdy=%d", requested);
    return 0;
}

/* XNU's node macros make permanent OIDs for kernel startup. Kexts register
 * non-permanent, versioned OIDs at run time and unregister them on stop. */
SYSCTL_DECL(_machdep);
static struct sysctl_oid bt_boot_oid,bt_firmware_oid,bt_power_on_oid,bt_uptime_oid,bt_baseline_oid,bt_lemask_oid,bt_lerdy_oid;
static bool bt_sysctls_registered;
static void bt_tty_register_oid(struct sysctl_oid *oid,const char *name,unsigned type,
                               sysctl_handler_t handler,const char *format,const char *description)
{
    bzero(oid,sizeof(*oid));
    oid->oid_parent=&sysctl__machdep_children;
    oid->oid_number=OID_AUTO;
    oid->oid_kind=(int)(type | CTLFLAG_RD | CTLFLAG_LOCKED | CTLFLAG_OID2);
    oid->oid_name=name;
    oid->oid_handler=handler;
    oid->oid_fmt=format;
    oid->oid_descr=description;
    oid->oid_version=SYSCTL_OID_VERSION;
    if(type==CTLTYPE_STRING) {
        oid->oid_arg1=bt_tty.firmware;
        oid->oid_arg2=sizeof(bt_tty.firmware);
    }
    sysctl_register_oid(oid);
}
void qbt_transport_unregister_sysctls(void)
{
    if(!bt_sysctls_registered)return;
    /* Unregistration waits for active handlers; do not hold bt_tty_lock. */
    sysctl_unregister_oid(&bt_lerdy_oid);
    sysctl_unregister_oid(&bt_lemask_oid);
    sysctl_unregister_oid(&bt_power_on_oid);
    sysctl_unregister_oid(&bt_firmware_oid);
    sysctl_unregister_oid(&bt_boot_oid);
    if(qbt_bt_trace_enabled)sysctl_unregister_oid(&bt_uptime_oid);
    if(qbt_bt_wait_baseline)sysctl_unregister_oid(&bt_baseline_oid);
    bt_sysctls_registered=false;
}

int qbt_transport_init(const struct qbt_transport_ops *ops)
{
    if(!ops || !ops->wake || !ops->power)return EINVAL;
    /* Claim before taking the lock: a second start must not initialize an
     * already live mutex, even while the first start is still publishing. */
    if(__atomic_exchange_n(&bt_tty_init_claimed,1u,__ATOMIC_ACQUIRE))return EALREADY;
    bt_tty_grp=lck_grp_alloc_init("unixnu.bt.tty",LCK_GRP_ATTR_NULL);
    if(!bt_tty_grp) {
        __atomic_store_n(&bt_tty_init_claimed,0u,__ATOMIC_RELEASE);
        return ENOMEM;
    }
    lck_mtx_init(&bt_tty_lock,bt_tty_grp,LCK_ATTR_NULL);
    BT_LOG("PianoBluetooth: bt-mutexfix1: transport lock initialized before publication");
    lck_mtx_lock(&bt_tty_lock);
    bt_ops=*ops;
    bool start=!bt_tty.node_thread;bt_tty.node_thread=true;
    lck_mtx_unlock(&bt_tty_lock);
    bt_tty_register_oid(&bt_boot_oid,"piano_bt_boot",CTLTYPE_INT,bt_tty_boot_sysctl,"I","WCN7860 kernel bring-up result");
    bt_tty_register_oid(&bt_firmware_oid,"piano_bt_firmware",CTLTYPE_STRING,sysctl_handle_string,"A","WCN7860 firmware build label");
    bt_tty_register_oid(&bt_power_on_oid,"piano_bt_power_on",CTLTYPE_INT,bt_tty_power_on_sysctl,"I","WCN7860 startup request");
    bt_tty_register_oid(&bt_lemask_oid, "piano_bt_lemask", CTLTYPE_INT|CTLFLAG_WR, bt_lemask_sysctl, "I",
                        "OR LE Data Length Change and PHY Update Complete into the host LE event mask (0 off, 1 on)");
    bt_tty_register_oid(&bt_lerdy_oid, "piano_bt_lerdy", CTLTYPE_INT|CTLFLAG_WR, bt_lerdy_sysctl, "I",
                        "Hand the host a no-change LE length/PHY event for a stalled LE link (0 off, 1 on)");
    if(qbt_bt_trace_enabled)
        bt_tty_register_oid(&bt_uptime_oid,"piano_bt_uptime_ms",CTLTYPE_QUAD,bt_uptime_sysctl,"Q","BT observation monotonic milliseconds");
    if(qbt_bt_wait_baseline)
        bt_tty_register_oid(&bt_baseline_oid, "piano_bt_reset_baseline", CTLTYPE_INT|CTLFLAG_WR, bt_baseline_sysctl, "I",
                            "Release the BT diagnostic startup gate after the host saves its baseline");
    bt_sysctls_registered=true;
    BT_LOG("PianoBluetooth: bt-path2: runtime sysctls registered");
    if(start && !bt_tty_make_node()) {
        thread_t thread=THREAD_NULL;
        if(kernel_thread_start(bt_tty_node_thread,NULL,&thread)==KERN_SUCCESS)thread_deallocate(thread);
    }
    return 0;
}

/* Called by the UART owner after each power transition: 0 after a complete bring-up (with the UART speed it configured),
 * else errno. */
void qbt_transport_power_result(bool enabled,int error,const char *firmware,uint32_t speed)
{
    (void)bt_tty_make_node();
    lck_mtx_lock(&bt_tty_lock);
    if(firmware) strlcpy(bt_tty.firmware,firmware,sizeof(bt_tty.firmware));
    if(enabled && !error){bt_tty.speed=speed;if(!bt_tty.open)bt_tty_termios_default();}
    bt_tty.boot=error?error:enabled?0:ENETDOWN;
    if(!enabled || error)bt_tty.rx_head=bt_tty.rx_len=bt_tty.tx_head=bt_tty.tx_len=0;
    bt_tty.power_pending=false;bt_tty.power_result=error;
    BT_LOG("[bluetooth tty] power=%u error=%d firmware=%s speed=%u",enabled,error,firmware?firmware:"",speed);
    wakeup(&bt_tty.boot);
    wakeup(&bt_tty.power_pending);
    /* Blocked clients observe the new state on their next pass. */
    bt_tty_wake_readers();bt_tty_wake_writers();
    lck_mtx_unlock(&bt_tty_lock);
    bt_kick();
}
void qbt_transport_boot_result(int error,const char *firmware,uint32_t speed)
{
    qbt_transport_power_result(true,error,firmware,speed);
}
