# RDR2 60fps Autopatch (PS5) — v1.00 fork

60 FPS for Red Dead Redemption 2 (PS4, `CUSA03041`) on a jailbroken PS5,
without installing any modded PKG and without touching the disk: the patch is
applied in memory each time the game boots. Nothing persists, so shutting the
console down leaves it clean.

> **This is a fork of [KurohaXR/rdr2-60fps-autopatch](https://github.com/KurohaXR/rdr2-60fps-autopatch).**
> The original targets **v1.32**. This fork's `src/autopatch60-oneshot.c`
> targets **v1.00** (clean base game, no update). If you play on 1.32, use the
> original repository instead. All credit for the tool, the approach and the
> testing methodology belongs to its author.

## What this fork changes

The 1.32 build checks a 64-byte context that only exists in the 1.32 eboot, so
it cannot work on 1.00. This fork changes the following:

- **Target:** `eboot+0x04a8ee1f` (RVA `0x468EE1F`) instead of `eboot+0x05853029`.
- **Verification:** instead of the 64-byte 1.32 context, the target must be a
  `cmove r32,r32` instruction (`0f 44 /r`, register-direct). The patch bytes
  (`xor r,r ; nop`) are derived from the register actually used, so on 1.00
  (`cmove esi,eax`) it writes the same `31 f6 90` as the 1.32 patch. If the
  bytes at the target are anything else, nothing is written.
- **Logging:** the 64 bytes around the target are written to
  `/mnt/usb0/autopatch60.log` (needs a USB drive), plus a line for each step.
  This makes wrong-address problems easy to diagnose.
- **Optional strict mode:** set `CTX_STRICT 1` and fill `CTX100[]` in the source
  to restore full 64-byte verification.

### Where the 1.00 address came from

The 60fps patch for 1.29 and 1.32 was known; 1.00 was not. In the
[r/ps4homebrew thread](https://www.reddit.com/r/ps4homebrew/comments/1hpoezt/),
the original poster (u/zekepliskin) explained that the Unlock FPS patch only
worked on 1.29 because the memory address differs on 1.00 and 1.32, and was
looking for the 1.00 address. **u/level3tjg** replied with `0x04a8ee1d`, which is
what made this port possible.

The address as posted was **2 bytes before** the instruction. Reading the live
bytes from a 1.00 process showed:

```
8d 70 ff       lea esi,[rax-1]
85 c0          test eax,eax        <- 0x04a8ee1d (address from the thread)
0f 44 f0       cmove esi,eax       <- 0x04a8ee1f (the actual target)
e8 ..          call
85 c0          test eax,eax
0f 88 ..       js
```

The surrounding code matches the 1.32 function, so the instruction and the
patch are the same as on 1.32. Only the address differs, and it ends in
`...1f`, not `...1d`. Thanks to u/level3tjg for the lead.

<!-- If a second Reddit user from the linked comment should be credited, add them here. -->

## Status — please read

- The tool verifies the original bytes, writes the patch and reads it back
  (`WRITE_OK` in the log, and the "RDR2 60fps ON" notification).
- On 1.00 the game feels smoother and CPU/GPU load goes up noticeably
  (roughly +15 CPU and +10 GPU in one tester's readout), which is consistent with
  a higher frame rate.
- **The output frame rate has not been independently measured.** A
  video frame-rate checker on a PS5 recording reported 29.97fps, but that
  reports the clip's recording rate, not necessarily what the game rendered.
  A capture card or high-speed camera is needed for proof.
- Treat 1.00 support as experimental. RDR2 was designed around 30fps; check for
  physics, animation or cutscene oddities and keep a save backup.

If you can measure the real frame rate (capture card / slow-motion camera),
please open an issue with your result.

## Requirements

- Jailbroken PS5 with `elfldr` (port 9021).
- [`ps5debug-NG`](https://github.com/OpenSourcereR-dev/ps5debug-NG) v1.3.0
  (server on port 744; the autopatcher is its client on `127.0.0.1`).
- RDR2 (PS4) `CUSA03041` **v1.00, clean base game with no update installed**.
- A Payload Manager with autoload (recommended) or manual loading of 2 ELFs.
- Optional: a USB drive (exFAT/FAT32) so the log can be written.

## Daily use (no PC)

1. After jailbreaking, load `ps5debug-NG.elf`, then `autopatch60-oneshot.elf`.
   Wait a few seconds.
2. **Then** launch RDR2 from the home screen. Start it fresh: don't resume from
   rest mode, and load the patcher before the game, because the write has to
   land in the first seconds of the game process.
3. The `RDR2 60fps ON - made by KurohaXR` notification appears.
4. When finished, close the game normally (Home → Options → Close). Nothing
   remains; the patch lived only in the dead game's RAM.

> ⚠️ Don't run `klogsrv` at the same time as `ps5debug-NG` (both listen on port 3232).

## Notifications and log lines

| Notification / log | Meaning |
|---|---|
| `RDR2 60fps ON` / `WRITE_OK` | Bytes checked, written and read back |
| `target not found, not applied` / `NO_TARGET` | Bytes at the target aren't a `cmove` (wrong game version?) |
| `unexpected eboot, not applied` / `ABORT_UNEXP` | Instruction found but not patchable |
| `write failed, not applied` / `WRITE_FAIL` | Write or read-back failed |
| `CONN_FAIL0` | `ps5debug-NG` isn't running |
| `TIMEOUT_NO_GAME` | The game didn't start within 10 minutes |

If something doesn't apply, the `CTX64` lines in the log show what the patcher
actually read at the target. Include them in any bug report.

## Building (once, on a PC with Linux/WSL, or in GitHub Codespaces)

```bash
sudo apt install clang-18 lld-18 make git wget unzip llvm-18-dev
# binary SDK:
sudo mkdir -p /opt && cd /tmp
wget https://github.com/ps5-payload-dev/sdk/releases/latest/download/ps5-payload-sdk.zip
sudo unzip -d /opt ps5-payload-sdk.zip
export LLVM_CONFIG=/usr/bin/llvm-config-18
export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk
cd src && make        # -> autopatch60-oneshot.elf
```

## About the original project

The original repository was written to avoid a crash caused by an update
package that bundled the 60fps patch (the post-boot background scan of that
package kernel-panicked jailbroken PS5s). Its approach is to play on the clean
game and unlock the frame rate in memory at boot with a 3-byte write through
`ps5debug-NG`, instead of installing a patched package. It also documents why
ItemzFlow's patch toggle did nothing on the author's console, and why a
resident (always-running) variant was dropped after a kernel trap at game
close. The full write-up is in the
[original README](https://github.com/KurohaXR/rdr2-60fps-autopatch).

The original author's scope note applies here too: this was built specifically
for RDR2. Porting to other titles or versions means reading that game's own
bytes, locating its own frame limiter and verifying it live.

## Disclaimer

Experimental software, no warranty of any kind. Provided "as is". Use it at your
own risk: it could crash the game (it should never touch the system, but nothing
is promised). The authors accept no responsibility for lost data, corrupted
saves or bans. If something breaks, restart the game; if the console misbehaves,
reboot it (nothing persists).

## Credits

- Original tool and method: **KurohaXR** —
  [rdr2-60fps-autopatch](https://github.com/KurohaXR/rdr2-60fps-autopatch).
- 1.32 *Unlock FPS* patch: **Jao**; patch repo
  [`illusionyy/PS-Game-Patch`](https://github.com/illusionyy/PS-Game-Patch) (**illusion**).
- 1.00 address lead: **u/level3tjg**, in the
  [r/ps4homebrew thread](https://www.reddit.com/r/ps4homebrew/comments/1hpoezt/)
  started by **u/zekepliskin**.
- [`ps5debug-NG`](https://github.com/OpenSourcereR-dev/ps5debug-NG) (**OpenSourcereR** et al.)
  + [`ps5-payload-sdk`](https://github.com/ps5-payload-dev/sdk),
  [`elfldr`](https://github.com/ps5-payload-dev/elfldr),
  `klogsrv`, `ftpsrv` (**John Törnblom** / ps5-payload-dev) — without these tools
  (and their authors) none of this would exist.
- Patch-format references: GoldHEN Cheat Repository, PS4 Cheats Manager (**bucanero**),
  ItemzFlow (**LightningMods**).

## License note

This repo's code is MIT (original copyright retained in `LICENSE`). Building
links against [`ps5-payload-sdk`](https://github.com/ps5-payload-dev/sdk) (GPL-3);
since the full source is published here, distributed binaries comply as long as
this same source goes with them.
