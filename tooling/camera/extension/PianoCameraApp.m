// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoCamera.app: the host of PianoCamera's camera extension (docs/drivers/camera.md section 4.5.2). No window; its commands print
// to stdout (as root over the USB root shell, from piano-camera-sysext's boot job, or by the logged-in user):
//   /Applications/PianoCamera.app/Contents/MacOS/PianoCamera activate [SECONDS]    submit the activation request; wait for its
//        result (default 60 s); "needs user approval" means Settings › General › Login Items & Extensions › Camera Extensions
//   ... deactivate [SECONDS]                                                         the deactivation request
//   ... properties                                                                   the extension's state as sysextd has it
// Exit status: 0 completed (or the properties were read), 3 waiting for the user's approval, 1 failed, 2 usage.
#import <Foundation/Foundation.h>
#import <SystemExtensions/SystemExtensions.h>

static NSString *const kExtension = @"com.unixnu.PianoCamera.Extension";

@interface Delegate : NSObject <OSSystemExtensionRequestDelegate>
@property(atomic) int status;
@property(atomic) BOOL done;
@end

@implementation Delegate
- (OSSystemExtensionReplacementAction)request:(OSSystemExtensionRequest *)request
                  actionForReplacingExtension:(OSSystemExtensionProperties *)existing
                                withExtension:(OSSystemExtensionProperties *)ext
{
  printf("PianoCamera: replacing version %s (%s) with %s (%s)\n", existing.bundleShortVersion.UTF8String, existing.bundleVersion.UTF8String,
         ext.bundleShortVersion.UTF8String, ext.bundleVersion.UTF8String);
  return OSSystemExtensionReplacementActionReplace;
}

- (void)requestNeedsUserApproval:(OSSystemExtensionRequest *)request
{
  printf("PianoCamera: the request needs the user's approval (Settings > General > Login Items & Extensions > Camera Extensions)\n");
  fflush(stdout);
  self.status = 3;
}

- (void)request:(OSSystemExtensionRequest *)request didFinishWithResult:(OSSystemExtensionRequestResult)result
{
  printf("PianoCamera: request finished: %s\n", result == OSSystemExtensionRequestCompleted ? "completed"
                                                 : result == OSSystemExtensionRequestWillCompleteAfterReboot ? "will complete after a reboot"
                                                                                                              : "unknown result");
  self.status = result == OSSystemExtensionRequestCompleted ? 0 : 4;
  self.done = YES;
}

- (void)request:(OSSystemExtensionRequest *)request didFailWithError:(NSError *)error
{
  printf("PianoCamera: request failed: %s %ld\n", error.domain.UTF8String, (long)error.code);
  self.status = 1;
  self.done = YES;
}

- (void)request:(OSSystemExtensionRequest *)request foundProperties:(NSArray<OSSystemExtensionProperties *> *)properties
{
  if (!properties.count) printf("PianoCamera: sysextd knows no %s\n", kExtension.UTF8String);
  for (OSSystemExtensionProperties *p in properties)
    printf("PianoCamera: %s %s (%s): enabled %d, awaiting approval %d, uninstalling %d\n", p.bundleIdentifier.UTF8String,
           p.bundleShortVersion.UTF8String, p.bundleVersion.UTF8String, p.isEnabled, p.isAwaitingUserApproval, p.isUninstalling);
  self.status = 0;
  self.done = YES;
}
@end

int main(int argc, const char *argv[])
{
  @autoreleasepool {
    setvbuf(stdout, NULL, _IOLBF, 0);
    const char *cmd = argc > 1 ? argv[1] : "properties";
    const double wait = argc > 2 ? atof(argv[2]) : 60;
    dispatch_queue_t q = dispatch_get_main_queue();
    OSSystemExtensionRequest *request = nil;
    if (!strcmp(cmd, "activate")) request = [OSSystemExtensionRequest activationRequestForExtension:kExtension queue:q];
    else if (!strcmp(cmd, "deactivate")) request = [OSSystemExtensionRequest deactivationRequestForExtension:kExtension queue:q];
    else if (!strcmp(cmd, "properties")) request = [OSSystemExtensionRequest propertiesRequestForExtension:kExtension queue:q];
    else {
      fprintf(stderr, "usage: PianoCamera activate|deactivate [SECONDS] | properties\n");
      return 2;
    }
    printf("PianoCamera: %s %s from %s (uid %d)\n", cmd, kExtension.UTF8String, NSBundle.mainBundle.bundlePath.UTF8String, getuid());
    Delegate *d = [[Delegate alloc] init];
    d.status = 1;
    request.delegate = d;
    [OSSystemExtensionManager.sharedManager submitRequest:request];
    NSDate *until = [NSDate dateWithTimeIntervalSinceNow:wait];
    while (!d.done && [until timeIntervalSinceNow] > 0)
      [[NSRunLoop mainRunLoop] runMode:NSDefaultRunLoopMode beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.25]];
    if (!d.done) printf("PianoCamera: no result within %.0f s%s\n", wait, d.status == 3 ? " (still waiting for the approval)" : "");
    return d.status;
  }
}
