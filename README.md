# DWPurple

DWPurple is an instant messenger in the style of Pidgin. It uses **libpurple 2.x** for
the protocols and **Dynamic Windows** for the user interface, so the same C sources build
for GTK 3 (Linux/BSD), OS/2 Presentation Manager, Windows and macOS.

## Features

- Buddy list with groups, contacts, chats, collapse/expand, offline/empty-group filters,
  status selector and status message
- Tabbed conversations (IM and chat), typing notifications, chat user list with
  op/voice prefixes, nick Tab-completion, `/commands`, sent-message history (Ctrl+Up/Down)
- Reaction notices ("Reacted with :x:") can be hidden; `:emoji_codes:` conversion,
  both per conversation (Options menu) with global defaults
- Emoji picker, send or paste an image (Ctrl+V, Shift+Insert or Conversation > Paste Image;
  inline when the protocol supports it, otherwise as a file)
- Account manager and editor (user splits, protocol options, registration)
- Join/add chat dialogs built from each protocol's chat fields
- Room list browser with filter, category expansion, join and add
- Privacy settings and block list; Block/Unblock from menus
- Buddy pounces (same `pounces.xml` actions as Pidgin)
- Sounds per event with automatic, command, beep or no playback
- System idle detection: Windows input, X11 screen saver extension, or activity in
  DWPurple windows
- System tray icon with status menu and unread indicator
- Desktop notifications for messages in background tabs (where Dynamic Windows supports them)
- File transfers, plugin manager (load, configure, actions), request/notify dialogs,
  debug window, preferences dialog
- Automatic reconnect with backoff after non-fatal disconnects

## Building on Linux (for example openSUSE Tumbleweed)

Requirements: GCC, GNU make, pkg-config, the libpurple 2.x development package, the
GLib development package, Dynamic Windows (installed, with `dwindows.pc` or
`dwindows-config`) and, optionally, the XScreenSaver development package.

```sh
sudo zypper install gcc make pkg-config libpurple-devel glib2-devel libXss-devel
make
./dwpurple
sudo make install            # /usr/local/bin/dwpurple, /usr/local/share/dwpurple/icons
```

If Dynamic Windows provides neither a `.pc` file nor `dwindows-config`:

```sh
make DW_CFLAGS="-I/usr/local/include" DW_LIBS="-L/usr/local/lib -ldwindows"
```

Add `HAVE_XSS=0` to build without the X11 screen saver extension.

## Building on OS/2

```sh
make -f Makefile.os2 DW_DIR=/path/to/dwindows
```

If pkg-config is not available, set `PURPLE_CFLAGS` and `PURPLE_LIBS` yourself. Copy
the `icons` directory next to `dwpurple.exe`. OS/2 has no built-in player for automatic
sound playback. Set the sound method to **Command** in Preferences, or the program beeps.
The tray icon requires a running SysTray/xCenter tray server.

## Command line

```
dwpurple [-c DIR] [-d] [-n] [-h] [-v]
  -c, --config DIR   configuration directory (default ~/.dwpurple)
  -d, --debug        print libpurple debug output to the terminal
  -n, --nologin      start with all accounts offline
```

The configuration directory is separate from Pidgin's `~/.purple` by default. To reuse
Pidgin's accounts, run `dwpurple -c ~/.purple` while Pidgin is closed.

## Files

| Path | Purpose |
|------|---------|
| `<config>/emoji.txt` | Custom emoji codes, one `code = emoji` per line |
| `<exe dir>/icons`, `<prefix>/share/dwpurple/icons`, `<config>/ui-icons` | Status, chat and application icons (PNG; ICO on Windows) |
| `<datadir>/sounds/purple`, `<exe dir>/sounds`, `<config>/sounds` | Default sound files (Pidgin's sounds are used if installed) |

## Limitations

- Message history is plain text by default (an MLE; formatting is removed and images
  appear as `[image: name]`). Preferences > Conversations > History display > Rich uses
  a render widget view instead, with inline images, on every platform including OS/2.
- No voice/video. libpurple media requires GStreamer and a UI that Dynamic Windows does not provide.
- Buddy icons are not shown.

## License

GPL version 2 or later, the same as libpurple. Dynamic Windows is distributed under its own
license.
