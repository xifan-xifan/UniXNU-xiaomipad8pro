#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Copyright (c) 2026, Leo Zhang. All rights reserved.
"""Actual native selection proof over CF fixture trees; no live AX transport."""
from pathlib import Path
import subprocess
import tempfile
import unittest
W=Path(__file__).resolve().parents[3]
class TreeTests(unittest.TestCase):
    def test_native_proof_rejects_unrelated_account_list_and_window(self):
        source=r'''
#include <ApplicationServices/ApplicationServices.h>
#include <assert.h>
static AXError fixture_copy(AXUIElementRef node,CFStringRef key,CFTypeRef *out) {
 CFTypeRef value=CFDictionaryGetValue((CFDictionaryRef)node,key);
 if(!value)return kAXErrorNoValue; *out=CFRetain(value);return kAXErrorSuccess;
}
static unsigned setters;
static AXError fixture_set(AXUIElementRef node,CFStringRef key,CFTypeRef value) {
 (void)node; (void)key; (void)value;
 ++setters;
 return kAXErrorAttributeUnsupported;
}
#define AXUIElementCopyAttributeValue fixture_copy
#define AXUIElementSetAttributeValue fixture_set
#define main production_main
#include "tooling/native/piano-login-readiness.c"
#undef main
static CFMutableDictionaryRef node(CFStringRef role) {
 CFMutableDictionaryRef n=CFDictionaryCreateMutable(NULL,0,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);
 CFDictionarySetValue(n,kAXRoleAttribute,role);return n;
}
static void children(CFMutableDictionaryRef n,const void **values,int count) {
 CFArrayRef a=CFArrayCreate(NULL,values,count,&kCFTypeArrayCallBacks);CFDictionarySetValue(n,kAXChildrenAttribute,a);CFRelease(a);
}
int main(void) {
 CFMutableDictionaryRef app=node(kAXApplicationRole),window=node(kAXWindowRole),other=node(kAXWindowRole);
 CFMutableDictionaryRef field=node(kAXTextFieldRole),label=node(kAXStaticTextRole),list=node(kAXListRole);
 CFDictionarySetValue(field,kAXSubroleAttribute,kAXSecureTextFieldSubrole);
 CFDictionarySetValue(field,kAXWindowAttribute,window);CFDictionarySetValue(label,kAXValueAttribute,CFSTR("fixture-user"));
 const void *windows[]={window,other};children(app,windows,2);
 const void *ready[]={label,field};children(window,ready,2);
 AXUIElementRef answer=selected_field((AXUIElementRef)window,CFSTR("fixture-user"),NULL);assert(answer);
 assert(focused_field((AXUIElementRef)app,answer,(AXUIElementRef)field,1));
 assert(setters==0);
 assert(!focused_field((AXUIElementRef)app,answer,(AXUIElementRef)label,0));
 assert(setters==0);
 assert(!focused_field((AXUIElementRef)app,answer,(AXUIElementRef)label,1));
 assert(setters==1);
 CFDictionarySetValue(app,kAXFocusedUIElementAttribute,field);
 assert(focused_field((AXUIElementRef)app,answer,(AXUIElementRef)label,1));
 assert(setters==2);
 CFRelease(answer);
 // Label in a different account/window must never authorize this field.
 const void *onlyfield[]={field},*onlylabel[]={label};children(window,onlyfield,1);children(other,onlylabel,1);
 assert(!selected_field((AXUIElementRef)window,CFSTR("fixture-user"),CFSTR("Fixture User")));
 // Even within one window, a visible chooser's account label is not selection.
 children(other,NULL,0);children(list,onlylabel,1);const void *chooser[]={list,field};children(window,chooser,2);
 assert(!selected_field((AXUIElementRef)window,CFSTR("fixture-user"),CFSTR("Fixture User")));
 // Correct window plus multiple secure fields is ambiguous and fails closed.
 CFMutableDictionaryRef second=node(kAXTextFieldRole);CFDictionarySetValue(second,kAXSubroleAttribute,kAXSecureTextFieldSubrole);
 const void *two[]={label,field,second};children(window,two,3);
 assert(!selected_field((AXUIElementRef)window,CFSTR("fixture-user"),CFSTR("Fixture User")));
}
'''
        with tempfile.TemporaryDirectory() as d:
            p=Path(d);(p/'tree.c').write_text(source)
            subprocess.run(['xcrun','clang','-Wall','-Wextra','-Werror','-I',str(W),str(p/'tree.c'),'-framework','ApplicationServices','-o',str(p/'tree')],check=True,capture_output=True)
            subprocess.run([str(p/'tree')],check=True,capture_output=True)
if __name__=='__main__':unittest.main()
