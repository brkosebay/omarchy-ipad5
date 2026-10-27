#!/bin/sh
# Orchestrator runs on Linux. Read existing sysfs/debugfs only; changes no policy.
set -eu
for path in /sys/devices/system/cpu/cpufreq/policy*/scaling_driver \
 /sys/devices/system/cpu/cpufreq/policy*/scaling_governor \
 /sys/devices/system/cpu/cpufreq/policy*/scaling_available_frequencies \
 /sys/devices/system/cpu/cpufreq/policy*/scaling_cur_freq \
 /sys/devices/system/cpu/cpufreq/policy*/cpuinfo_cur_freq \
 /sys/devices/system/cpu/cpufreq/policy*/stats/time_in_state \
 /sys/devices/system/cpu/cpuidle/current_driver \
 /sys/devices/system/cpu/cpu*/cpuidle/state*/name \
 /sys/devices/system/cpu/cpu*/cpuidle/state*/usage \
 /sys/devices/system/cpu/cpu*/cpuidle/state*/time \
 /sys/kernel/debug/pm_genpd/pm_genpd_summary \
 /sys/class/thermal/thermal_zone*/type /sys/class/thermal/thermal_zone*/temp \
 /sys/class/power_supply/*/uevent /proc/interrupts; do
    [ -r "$path" ] || continue
    printf '\n%s\n' "$path"
    cat "$path"
done
