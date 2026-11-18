---
id: TP-056
tool: "tools/ref-capture"
status: building
proposed_by: "Claude Opus 5.5 (driver, oriole unattended run)"
date: 2026-11-14
exposed_by: "oriole touch bring-up: the success criteria were guessed until the stock driver's state was read in recovery (oriole-recovery1, by hand); the 2026-11-14 plan step I1 needs the same reference for Wi-Fi, GPU, charging, BT and audio"
extends: ""
related: "TP-031"
usage: "tools/ref-capture [--out DIR] [--only NAME,...] [--list]"
refs: "-"
---

## checked
`tools/tool-proposals check ref-capture` / `check recovery`: no tool, no proposal. The earlier stock reference (`~/.hermes/cache/scratch/oriole-recovery1/dmesg.txt`) was taken by hand with ad-hoc `adb shell` lines; one of those (`/sys/kernel/debug/clk/clk_summary`) crashed the phone [obs, user]. TP-031 reads fastboot variables only.

## why
Every area of the plan (Wi-Fi, GPU, charging, BT, speakers) needs the stock Linux state of this phone (pinctrl, regulators, power supplies, device tree, interrupts) as its success criterion. By hand that is ~20 commands per session, each a chance to read a crashing debugfs file. One bounded, allowlisted capture removes that and makes the reference reproducible.

## output
One line per item (`OK name bytes`, `EMPTY name`, `TIMEOUT name`, `REFUSED name`), then `RESULT: PASS files=N dir=DIR` (exit 0: no item timed out and N > 0), `RESULT: FAIL ...` (exit 1: an item timed out or nothing was captured), `RESULT: ERROR ...` (exit 2: no adb device, not recovery, or `ro.product.device` is not `$BOARD`). Files land in `~/pixelios-artifacts/ref/<board>/<UTC stamp>/` (never in git: firmware-adjacent state, possibly serials in dmesg).

## risks
Touches only the attached phone in recovery whose `ro.product.device` equals `$BOARD` (AGENTS.md rule 2). Read-only: a fixed allowlist of commands, each run as `timeout 8` on the phone plus a host timeout. A hard-coded denylist refuses `clk/clk_summary` (crashed the phone once), any `regmap/*/registers` and anything with an output redirection or a write verb; the check runs on every command before it is sent (tested with a fake adb). Never reboots the phone.

## amendments
(none)
