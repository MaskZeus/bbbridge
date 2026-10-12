# bbbridge (C++)

Play **co-op with bbhost players** on **The Hunter's Dream** while using
**bbport** (Supermedo's `bloodborne_pc` for Windows, DLSS / FSR 3.1 / FSR 4).

bbbridge looks like a shadNet server to bbport and talks to The Hunter's Dream
the way bbhost does: matching (rooms, bells, summons), the P2P traffic between
the two clients (different packet framing, bbport's signaling handshake,
The Hunter's Dream's STUN and relay), and the game's own server traffic
(account token, play log for the live map). bbport itself is not modified.

## Layout

```
E:\bloodborne\               bbport
└─ bbbridge\                 this folder
   ├─ bbbridge.exe           the app (built)
   ├─ bbbridge_config.json   settings (optional)
   ├─ bbbridge_state.json    your account: token + recovery code - never share it
   ├─ bbbridge.log           log of the last run (previous: bbbridge.prev.log)
   ├─ src\                   C++ sources
   ├─ tests\                 unit tests (C++) and end-to-end tests (Python)
   ├─ third_party\           nlohmann/json 3.11.3, Dear ImGui 1.91.5
   └─ bin\                   bbbridge-cli.exe (headless), bbbridge-unit.exe
```

## Using it

1. Put the `bbbridge` folder inside bbport's folder (next to `Bloodborne.exe`)
   and start `bbbridge.exe` (close the bbport launcher first). bbbridge uses
   the bbport it sits in; anywhere else it asks which bbport to use
   (**Change...** picks another one later).
2. **Sign in with Discord**: the app shows a code and opens The Hunter's Dream;
   sign in there with your Discord account and approve the code. Your account
   there (the same one bbhost uses) becomes bbport's online id. No password is
   stored; the account's token goes to `bbbridge_state.json`.
3. Tick **Route bbport through bbbridge** (and the live map patch if you like).
4. Press **PLAY**. The app starts the bridge if needed and launches the game
   with bbport's saved settings. You can also start the game from the bbport
   launcher - as long as bbbridge is open. Graphics, mods and cheats stay in
   the bbport launcher (**bbport settings...**) or the in-game menu (Insert).

Untick **Route bbport through bbbridge** to go back to bbport's own shadNet
co-op pool: bbport gets its own server and shadNet account back (the ones it
had before routing; a shadNet account is registered at
[shadnet.shadps4.net](https://shadnet.shadps4.net) and entered in the bbport
launcher's *Online co-op*). PLAY follows the box: ticked, the bbhost pool with
your Discord sign-in; unticked, bbport's own shadNet - no sign-in needed.
Running bbhost and bbport with the same account at the same time is not
supported.

## Building (Visual Studio 2022/2026)

Workload **Desktop development with C++** (MSVC, Windows SDK, CMake tools).

- Visual Studio: *File > Open > Folder...* this folder, pick the **x64 Release**
  preset, *Build > Build All*. `bbbridge.exe` is copied here, the CLI and the
  unit tests to `bin\`.
- Command line (Developer PowerShell):
  `cmake --preset x64-release` then `cmake --build --preset x64-release`

## Tests

- `bin\bbbridge-unit.exe` - framing, signaling, STUN, protobuf, account id.
- `python tests\e2e\test_e2e.py` - the bridge exe against a fake bbhost
  server, a fake STUN/relay server and fake game sockets (no internet).

## Credits and license

The protocol comes from **bbhost** and The Hunter's Dream by droogie (GPL-3.0);
**bloodborne_pc / bbport** by Supermedo on deadinside28's port and shadPS4.
bbbridge is GPL-3.0-or-later. nlohmann/json and Dear ImGui are MIT-licensed.
Not affiliated with Sony, FromSoftware, Supermedo or The Hunter's Dream.
