# RootPolicies ADT comparison

UbuntuPiano used the UniXNU generator from checkout `d830d23b0d19c9322d906c21afeca1d6c54aeffe` with local changes to `scripts/unixnu/devicetree.py`. The checkout's base matches `unixnu.lock`.

With `configs/k273-adt.toml` and a synthetic chip ID, two control runs produced 15,736-byte ADTs. A third run with `--root-policies` produced 15,788 bytes. The parsed property comparison consumed each entire file. All three had distinct `/chosen/random-seed` values because the generator requests fresh randomness on every run. The third output added `/chosen/memory-map/RootPolicies` with exactly 16 zero bytes. Every other property value matched. This was a host test.
