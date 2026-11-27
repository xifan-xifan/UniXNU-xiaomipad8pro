// SPDX-License-Identifier: MIT
#import "display_evidence.h"

int main(int argc, const char *argv[])
{
    @autoreleasepool {
        if (argc != 3 && argc != 4)
            return 2;
        return ft_kernel_report(argv[1], argv[2], argc == 4 ? argv[3] : NULL);
    }
}
