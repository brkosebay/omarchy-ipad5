# Next iOS session: read-only power observation

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


**Historical workflow, superseded by [ios-round3.md](ios-round3.md).** The
fixed idle/render windows did not fit the owner's workflow. Round3 signs with
an explicit IOReport entitlement file, captures CFErrors and records one
on-demand window per command. Do not use the old ios-sample.sh invocation below.

> Private graphics-dependent excerpt omitted from this source snapshot.

> Private graphics-dependent excerpt omitted from this source snapshot.

Run these exact commands from the workspace on the Mac once iOS is booted:

```sh
scripts/power/build-ios-ioreport.sh
scripts/ipad-ssh.sh 'mkdir -p /var/mobile/linux-a9-power-r2'
IPAD_SCP=1 scripts/ipad-ssh.sh -O scripts/power/ios-observe.sh scripts/power/ios-sample.sh build/power/ios-ioreport mobile@127.0.0.1:/var/mobile/linux-a9-power-r2/
scripts/ipad-ssh.sh 'chmod 700 /var/mobile/linux-a9-power-r2/ios-ioreport; cd /var/mobile/linux-a9-power-r2 && sh ios-observe.sh observe-1'
scripts/ipad-ssh.sh 'cd /var/mobile/linux-a9-power-r2 && sh ios-sample.sh sample-1 /var/mobile/linux-a9-power-r2/ios-ioreport'
mkdir -p build/power/ios-r2
IPAD_SCP=1 scripts/ipad-ssh.sh -O -r mobile@127.0.0.1:/var/mobile/linux-a9-power-r2/observe-1 mobile@127.0.0.1:/var/mobile/linux-a9-power-r2/sample-1 build/power/ios-r2/
find build/power/ios-r2 -type f -exec shasum -a 256 {} \; > build/power/ios-r2-SHA256SUMS
```

During sampling leave brightness fixed. First ten seconds idle, middle ten
seconds manually scroll/animate the same chosen app, final ten seconds idle.
The terminal prints each phase. This is a short correlation capture, not a
battery-life benchmark; USB charging and IORegistry traffic confound power.
Record the app, brightness and cable/charging state with the pulled files.

No sudo or device password is needed by the prepared tools. If exports,
entitlements, channels or `ioreg` are unavailable, pull exact errors. If
signing was unavailable on the host the build script says so; install `ldid`
and rerun the host build before upload rather than running unsigned code.
The successful host build in this round used `ldid -S`.

Output directories must be **new**: a second run uses `observe-2`/`sample-2`
and corresponding pull paths. The scripts use `umask 077`, refuse existing
output directories and never delete prior evidence. INT/TERM handling stops
only sampler child PIDs started by that script. Each sampler is also bounded
by count; no background service survives normal completion. If SSH drops,
wait for the bounded run and inspect status/logs before starting another.

Inspect `sample-1/status.txt`, all three `*.log` files, `channels.plist`,
`subscribed.plist` and phase timestamps before interpreting deltas. Channel
legends provide units/state IDs; do not infer watts from undocumented IOReport
counters. Any requested further selectors or kernel reads need a separate
reviewed implementation; these scripts deliberately expose no such option.
