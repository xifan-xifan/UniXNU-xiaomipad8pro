// SPDX-License-Identifier: MIT
#import <Foundation/Foundation.h>

NSDictionary *ft_read_snapshot(void);
NSDictionary *ft_scanout_report(NSDictionary *before, NSDictionary *after);
int ft_kernel_report(const char *before, const char *after, const char *log);
NSDictionary *ft_gop_properties(void);
BOOL ft_write_json(NSDictionary *report);
