# On-demand iOS power recording — round 3

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


> Private graphics-dependent excerpt omitted from this source snapshot.

## Deploy the signed helper and wrapper

Use the existing session directory; every capture subdirectory must be new:

```sh
scripts/power/build-ios-ioreport.sh
scripts/ipad-ssh.sh 'mkdir -p /var/mobile/linux-a9-power-r2'
IPAD_SCP=1 scripts/ipad-ssh.sh -O scripts/power/ios-sample.sh scripts/power/ios-observe.sh build/power/ios-ioreport mobile@127.0.0.1:/var/mobile/linux-a9-power-r2/
scripts/ipad-ssh.sh 'chmod 700 /var/mobile/linux-a9-power-r2/ios-ioreport'
```

The build fails if signing is unavailable. `build/power/ios-ioreport.plist`
is extracted from the signed binary and checked against
`configs/power/ios-ioreport.plist`. Both observed daemon IOKit entitlement
spellings contain IOReportUserClient. The helper retains the working
platform/no-container/no-sandbox launch envelope. It copies no unrelated
power-daemon privileges or seatbelt profile selector.

Optional quick preflight before the owner starts a workload:

```sh
scripts/ipad-ssh.sh 'cd /var/mobile/linux-a9-power-r2 && sh ios-sample.sh preflight preflight-r3-1 ./ios-ioreport'
```

## Record now, whenever the owner is ready

The owner starts rendering/scrolling first. The orchestrator then runs this
single command for ten nominal seconds; no phase transition is scheduled:

```sh
scripts/ipad-ssh.sh 'cd /var/mobile/linux-a9-power-r2 && sh ios-sample.sh record 10 render record-r3-1 ./ios-ioreport'
```

For a separately chosen idle window, once the owner has stopped activity:

```sh
scripts/ipad-ssh.sh 'cd /var/mobile/linux-a9-power-r2 && sh ios-sample.sh record 10 idle idle-r3-1 ./ios-ioreport'
```

Use a new output name for each attempt (record-r3-2, idle-r3-2, etc.). N can be
1–300. Maintain the chosen activity throughout the command. Brightness,
foreground app, charging/cable state and temperature should be recorded with
the evidence. The three groups have independent timestamps; elapsed sample
times include measurable IOReport/serialization overhead. This is correlation
telemetry, not a battery-life benchmark.

## Pull the result and classify failures

```sh
mkdir -p build/power/ios-r3
IPAD_SCP=1 scripts/ipad-ssh.sh -O -r mobile@127.0.0.1:/var/mobile/linux-a9-power-r2/record-r3-1 build/power/ios-r3/
```

> Private graphics-dependent excerpt omitted from this source snapshot.

On failure, each group logs the API stage, uid/euid and CFError domain/code/
reason, and writes `error.plist` if the library supplies an error. Exit6 means
subscription creation failed; exit7 means initial/sample/delta creation
failed. Neither code by itself proves an entitlement denial. Pull the entire
directory even if SSH exits nonzero. Do not repeat the three timed phases
from round2 or interpret channel legends as sampled values.

> Private graphics-dependent excerpt omitted from this source snapshot.

> Private graphics-dependent excerpt omitted from this source snapshot.


If all groups report `Could not open IOReportHub`, first retain the exact
error and verify the newly deployed signed binary. That is an access-stage
failure, not evidence for changing channel flags. An observe snapshot includes
the hub and user-client properties:

```sh
scripts/ipad-ssh.sh 'cd /var/mobile/linux-a9-power-r2 && sh ios-observe.sh observe-r3-1'
```

## Single optional daemon/profile pull

The orchestrator already supplied five daemon binaries for this round. To
collect another capture including readable profile/launchd files in one go,
choose a **new** host directory (the existing flat `daemons/` is preserved):

```sh
scripts/power/pull-ios-daemons.sh --list
scripts/power/pull-ios-daemons.sh build/power/ios-r3/daemons-next-1
```

The first command only lists paths. The second performs the bounded read-only
pull and host entitlement extraction. Absent optional tools/frameworks are
recorded, not treated as installed because a dyld-cache string names them.
There is no sudo or DMG download. Inspect manifest.tsv, entitlement-summary.json,
the original payload binaries and the extracted entitlements before changing
the signing file. All raw device captures remain private/gitignored.
