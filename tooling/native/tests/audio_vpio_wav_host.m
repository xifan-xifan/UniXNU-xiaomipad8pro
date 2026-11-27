/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#define main vpio_main
#include "../piano-vpio-probe/piano-vpio-probe.m"
#undef main

int main(int argc, char **argv)
{
    if (argc != 3 || strcmp(argv[1], "--load-wav"))
        return vpio_main(argc, argv);

    float *samples = NULL;
    uint64_t frames = 0;
    double rate = 0;
    if (load_wav(argv[2], &samples, &frames, &rate))
        return 1;
    printf("%.0f Hz %llu frames first %.4f\n", rate, (unsigned long long)frames, samples[0]);
    free(samples);
    return 0;
}
