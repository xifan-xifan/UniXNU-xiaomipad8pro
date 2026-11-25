// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026, Leo Zhang. All rights reserved.
// Read only UI proof, except prepare focuses the already selected account's
// secure field. No password value is read or printed. AX denial fails closed.
#include <ApplicationServices/ApplicationServices.h>
#include <libproc.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static CFTypeRef attr(AXUIElementRef e, CFStringRef key)
{
  CFTypeRef value = NULL;
  if (AXUIElementCopyAttributeValue(e, key, &value) != kAXErrorSuccess) return NULL;
  return value;
}
static int equal(CFTypeRef v, CFStringRef wanted)
{ return wanted && v && CFGetTypeID(v) == CFStringGetTypeID() && CFEqual(v, wanted); }
static int secure(AXUIElementRef e)
{
  CFTypeRef v = attr(e, kAXSubroleAttribute);
  int ok = equal(v, kAXSecureTextFieldSubrole);
  if (v) CFRelease(v);
  return ok;
}
static const char *focus_kind(AXUIElementRef focused)
{
  if (!focused) return "focus-unavailable";
  if (secure(focused)) return "focus-secure";
  CFTypeRef role = attr(focused, kAXRoleAttribute);
  const char *kind = "focus-other";
  if (equal(role, kAXTextFieldRole)) kind = "focus-text";
  else if (equal(role, kAXButtonRole)) kind = "focus-button";
  else if (equal(role, kAXGroupRole)) kind = "focus-group";
  if (role) CFRelease(role);
  return kind;
}
static void walk(AXUIElementRef e, CFStringRef shortname, CFStringRef fullname,
                 unsigned depth, unsigned *budget, int *account, AXUIElementRef *field, unsigned *fields, int *chooser)
{
  if (!*budget) return;
  if (depth > 12) { *budget = 0; return; }
  --*budget;
  if (secure(e)) { ++*fields; if (!*field) *field = (AXUIElementRef)CFRetain(e); return; }
  CFTypeRef role = attr(e, kAXRoleAttribute);
  if (equal(role, kAXListRole) || equal(role, kAXTableRole) || equal(role, kAXOutlineRole) || equal(role, kAXRadioGroupRole)) *chooser = 1;
  if (equal(role, kAXStaticTextRole) || equal(role, kAXImageRole)) {
    const CFStringRef keys[] = { kAXTitleAttribute, kAXDescriptionAttribute, kAXValueAttribute };
    const unsigned count = equal(role, kAXStaticTextRole) ? 3 : 2;
    for (unsigned n = 0; n < count && !*account; ++n) {
      CFTypeRef v = attr(e, keys[n]);
      if (equal(v, shortname) || equal(v, fullname)) *account = 1;
      if (v) CFRelease(v);
    }
  }
  if (role) CFRelease(role);
  CFTypeRef children = attr(e, kAXChildrenAttribute);
  if (children && CFGetTypeID(children) == CFArrayGetTypeID())
    for (CFIndex i = 0; *budget && i < CFArrayGetCount(children); ++i)
      walk((AXUIElementRef)CFArrayGetValueAtIndex(children, i), shortname, fullname, depth + 1, budget, account, field, fields, chooser);
  if (children) CFRelease(children);
}
// Account labels elsewhere in the app, or a chooser showing several accounts,
// cannot identify the account belonging to this password field.
static AXUIElementRef selected_field(AXUIElementRef window, CFStringRef shortname, CFStringRef fullname)
{
  CFTypeRef role = attr(window, kAXRoleAttribute);
  int is_window = equal(role, kAXWindowRole);
  if (role) CFRelease(role);
  if (!is_window) return NULL;

  int account = 0, chooser = 0; unsigned budget = 500, fields = 0;
  AXUIElementRef field = NULL;
  walk(window, shortname, fullname, 0, &budget, &account, &field, &fields, &chooser);
  if (account && !chooser && budget && fields == 1) return field;
  if (field) CFRelease(field);
  return NULL;
}

static int focused_field(AXUIElementRef app, AXUIElementRef field, AXUIElementRef focused, int prepare)
{
  if (focused && CFEqual(focused, field) && secure(focused)) return 1;
  if (!prepare) return 0;
  // Some AX implementations reject a focus setter even when focus changes.
  AXUIElementSetAttributeValue(field, kAXFocusedAttribute, kCFBooleanTrue);
  AXUIElementRef current = (AXUIElementRef)attr(app, kAXFocusedUIElementAttribute);
  int ok = current && CFEqual(current, field) && secure(current);
  if (current) CFRelease(current);
  return ok;
}

static int focus_probe(const char *mode, const char *shortname, const char *fullname)
{
  struct stat st;
  if (stat("/dev/console", &st) || st.st_uid != 0) {
    puts("focus_probe=not-at-login-window"); return 1;
  }
  alarm(10);
  AXUIElementRef system = AXUIElementCreateSystemWide();
  AXUIElementSetMessagingTimeout(system, 2);
  CFTypeRef front = NULL;
  AXError error = AXUIElementCopyAttributeValue(system, kAXFocusedApplicationAttribute, &front);
  if (error != kAXErrorSuccess || !front) {
    printf("focus_probe=front-app-unavailable ax=%d\n", error); return 1;
  }
  AXUIElementRef app = (AXUIElementRef)front;
  pid_t pid = 0; char name[64] = {};
  if (AXUIElementGetPid(app, &pid) || proc_name(pid, name, sizeof name) <= 0) {
    puts("focus_probe=front-process-unavailable"); return 1;
  }
  if (!strcmp(name, "SecurityAgent")) puts("focus_probe=front-securityagent");
  else if (!strcmp(name, "loginwindow")) puts("focus_probe=front-loginwindow");
  else { puts("focus_probe=front-not-login-ui"); return 1; }
  fflush(stdout);
  AXUIElementSetMessagingTimeout(app, 2);
  AXUIElementRef focused = (AXUIElementRef)attr(app, kAXFocusedUIElementAttribute);
  printf("focus_probe=%s\n", focus_kind(focused));
  fflush(stdout);
  AXUIElementRef window = (AXUIElementRef)attr(app, kAXFocusedWindowAttribute);
  if (!window && focused && secure(focused)) window = (AXUIElementRef)attr(focused, kAXWindowAttribute);
  if (!window) { puts("focus_probe=window-unavailable"); return 1; }
  CFStringRef short_label = CFStringCreateWithCString(NULL, shortname, kCFStringEncodingUTF8);
  CFStringRef full_label = fullname[0] ? CFStringCreateWithCString(NULL, fullname, kCFStringEncodingUTF8) : NULL;
  AXUIElementRef field = selected_field(window, short_label, full_label);
  if (!field) { puts("focus_probe=account-or-field-unconfirmed"); return 1; }
  if (!focused_field(app, field, focused, !strcmp(mode, "prepare"))) {
    puts("focus_probe=secure-focus-unconfirmed"); return 1;
  }
  CFTypeRef still_front = attr(system, kAXFocusedApplicationAttribute);
  if (!still_front || !CFEqual(still_front, app) || !focused_field(app, field, (AXUIElementRef)attr(app, kAXFocusedUIElementAttribute), 0)) {
    puts("focus_probe=focus-changed"); return 1;
  }
  puts("focus_probe=secure-focus-confirmed");
  puts("selected_login_account=yes\nfocused_password_field=yes");
  return 0;
}

int main(int argc, char **argv)
{
  if (argc == 3 && !strcmp(argv[1], "desktop")) {
    uid_t uid = (uid_t)strtoul(argv[2], NULL, 10);
    struct stat st;
    if (!uid || stat("/dev/console", &st) || st.st_uid != uid) return 1;
    puts("console_user_matches=yes");
    return 0;
  }
  if (argc < 2 || (strcmp(argv[1], "prepare") && strcmp(argv[1], "ready")) || geteuid()) {
    puts("focus_probe=invalid-invocation");
    return 2;
  }
  if (argc == 5 && !strcmp(argv[2], "--in-login-session"))
    return focus_probe(argv[1], argv[3], argv[4]);
  if (argc != 4 || argv[3][0] == '\0' || argv[3][strspn(argv[3], "0123456789")]) {
    puts("focus_probe=invalid-invocation");
    return 2;
  }

  // Directory Services need the original bootstrap. AX needs the login
  // bootstrap. Carry just the resolved account labels across the exec.
  uid_t uid = (uid_t)strtoul(argv[2], NULL, 10);
  struct passwd *account = uid ? getpwuid(uid) : NULL;
  if (!account || !account->pw_name || !account->pw_name[0]) {
    puts("focus_probe=account-lookup-failed");
    return 2;
  }
  const char *launchctl = "/bin/launchctl";
  char *args[] = {(char *)launchctl, "bsexec", argv[3], argv[0], argv[1],
                  "--in-login-session", account->pw_name, account->pw_gecos ?: "", NULL};
  execv(launchctl, args);
  puts("focus_probe=session-exec-failed");
  return 2;
}
