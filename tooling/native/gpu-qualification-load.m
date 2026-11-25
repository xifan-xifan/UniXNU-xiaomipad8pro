// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Leo Zhang
// Own-window redraw load: no AppleEvents, Accessibility, or Screen Recording permission.
// Build as an arm64 Cocoa app; launch in a proven GUI session with `open Redraw.app`.
#import <Cocoa/Cocoa.h>
#include <math.h>
#include <sys/sysctl.h>
static unsigned long long draws;
@interface LoadView : NSView
@property NSUInteger phase;
@end
@implementation LoadView
- (void)drawRect:(NSRect)dirty {
    (void)dirty;++draws;
    [[NSColor colorWithCalibratedHue:(self.phase%360)/360.0 saturation:.65 brightness:.8 alpha:1] setFill];
    NSRectFill(self.bounds);
    for(NSUInteger i=0;i<12;++i) {
        [[NSColor colorWithCalibratedHue:((self.phase+i*27)%360)/360.0 saturation:.7 brightness:.9 alpha:1] setFill];
        NSRect r=NSMakeRect((i%4)*self.bounds.size.width/4,(i/4)*self.bounds.size.height/3,
                            self.bounds.size.width/4-3,self.bounds.size.height/3-3);
        [[NSBezierPath bezierPathWithOvalInRect:r] fill];
    }
    NSString *s=[NSString stringWithFormat:@"R3a redraw %lu",(unsigned long)self.phase];
    [s drawAtPoint:NSMakePoint(12,12) withAttributes:@{NSFontAttributeName:[NSFont boldSystemFontOfSize:22],NSForegroundColorAttributeName:NSColor.whiteColor}];
}
@end
@interface LoadDelegate : NSObject <NSApplicationDelegate>
@property NSMutableArray<NSWindow *> *windows;
@property NSTimer *timer;
@property double start;
@property double thermalExitAt;
@property BOOL moderate;
@property unsigned long long moves, ticks;
@end
@implementation LoadDelegate
- (void)applicationDidFinishLaunching:(NSNotification *)n {
    (void)n;self.start=NSProcessInfo.processInfo.systemUptime;self.windows=[NSMutableArray array];
    self.moderate=[NSUserDefaults.standardUserDefaults boolForKey:@"pianoQualificationModerate"];
    NSUInteger windowCount=self.moderate?2:6;
    NSRect screen=NSScreen.mainScreen.visibleFrame;
    for(NSUInteger i=0;i<windowCount;++i) {
        NSWindow *w=[[NSWindow alloc] initWithContentRect:NSMakeRect(screen.origin.x+60+i*45,screen.origin.y+70+i*40,
            screen.size.width*.28,screen.size.height*.36) styleMask:NSWindowStyleMaskTitled|NSWindowStyleMaskClosable
            backing:NSBackingStoreBuffered defer:NO];
        w.title=[NSString stringWithFormat:@"R3a owned window %lu",(unsigned long)i];
        w.contentView=[[LoadView alloc] initWithFrame:w.contentView.bounds];
        [w orderFront:nil];[self.windows addObject:w];
    }
    [NSApp activateIgnoringOtherApps:YES];
    self.timer=[NSTimer scheduledTimerWithTimeInterval:1.0/30 target:self selector:@selector(tick:) userInfo:nil repeats:YES];
}
- (void)tick:(NSTimer *)timer {
    (void)timer;double elapsed=NSProcessInfo.processInfo.systemUptime-self.start;++self.ticks;
    NSFileManager *files=NSFileManager.defaultManager;
    int guard=0;size_t guardBytes=sizeof guard;
    bool exitGuard=sysctlbyname("machdep.piano_gpu_qual607",&guard,&guardBytes,NULL,0)==0 && guard>=0 && (guard&8) &&
        ((guard>>8)!=1 || ![files fileExistsAtPath:@"/tmp/gpur3a/allow-fallback"]);
    if(self.moderate && exitGuard) {
        int reason=guard>>8;
        // Keep the same load for the capped comparison after expiry. For a thermal trip, allow a short recovery observation.
        if(reason==5)exitGuard=false;
        if(reason==2 || reason==3) {
            if(!self.thermalExitAt)self.thermalExitAt=elapsed;
            exitGuard=elapsed-self.thermalExitAt>=20;
        }
    }
    if(exitGuard || elapsed>=1000 || [files fileExistsAtPath:@"/tmp/gpur3a-stop"]) {
        // A same-directory image lets the companion file:// page observe the stop without a server or Automation.
        [files copyItemAtPath:@"/tmp/gpuoom/pattern0.png" toPath:@"/tmp/gpur3a/stop.png" error:NULL];
        [NSApp terminate:nil];return;
    }
    NSRect screen=NSScreen.mainScreen.visibleFrame;
    for(NSUInteger i=0;i<self.windows.count;++i) {
        NSWindow *w=self.windows[i];NSRect f=w.frame;
        f.origin.x=screen.origin.x+(screen.size.width-f.size.width)*(.5+.42*sin(elapsed*.67+i));
        f.origin.y=screen.origin.y+(screen.size.height-f.size.height)*(.5+.42*cos(elapsed*.53+i));
        [w setFrame:f display:YES];++self.moves;
        LoadView *v=(LoadView *)w.contentView;v.phase=(NSUInteger)(elapsed*40)+i*30;[v setNeedsDisplay:YES];
    }
    if(self.ticks%30==0) {
        FILE *f=fopen("/tmp/gpur3a-redraw.txt","a");
        if(f) {fprintf(f,"REDRAW elapsed=%.3f moves=%llu draws=%llu windows=%lu ticks=%llu\n",elapsed,self.moves,draws,
            (unsigned long)self.windows.count,self.ticks);fclose(f);}
    }
}
@end
int main(void) {
    @autoreleasepool {NSApplication *app=NSApplication.sharedApplication;LoadDelegate *delegate=[LoadDelegate new];
        app.delegate=delegate;[app setActivationPolicy:NSApplicationActivationPolicyRegular];[app run];}
    return 0;
}
