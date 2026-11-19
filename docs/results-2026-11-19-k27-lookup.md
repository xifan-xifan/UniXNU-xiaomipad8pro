# Locked k27 Source Lookup

Audit date: 2026-11-19. This is a read-only GitHub source-discovery check. No device access, backup, or write was performed.

The required local native-variant input remains commit `11ac316b789d81fc9f734e15645456f536c05141`, branch `el1/native-variant`, with XNU patches `0001-0054` and prepared tree `de16623d957cccff6dfb78d8eca4a79842af9702`.

The live `Droid-VM/macOS-DroidVM` refs are `main` at `524d27bf57ae92375aa39e3a81e11727c08973ff`, `macOS` at `854aac22da246ae8ddacf253e19ad9b04c37d2dd`, and `wip/hackintosh` at `f7d070fe55d01c08f3a5862ba446887968679773`. The GitHub API all-state pull request query returned no pull requests.

The current GitHub fork API returned these eight repositories; a direct commit lookup for the required native pin did not find it in any of them:

- `a19102275-jpg/macOS-DroidVM`
- `qeqenn/macOS-DroidVM`
- `zhangxucheng018/macOS-DroidVM`
- `djun/macOS-DroidVM`
- `bushan/macOS-DroidVM`
- `1454228/macOS-DroidVM`
- `msft404/macOS-DroidVM`
- `rooteduniverse1003/macOS-DroidVM`

This lookup found no new source candidate. The separate local DroidVM 27.0.1 tree remains useful provenance for the recorded VM runtime successes, but it is not the required native 27.3 source. No dependency pin or build input was changed.
