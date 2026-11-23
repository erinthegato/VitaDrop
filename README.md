# VitaDrop

Send files from your iPhone to your PS Vita. Open the app on the Vita, scan the QR code with your iPhone camera, pick files, and they get copied over Wi-Fi. You don't need to install anything on the iPhone.

Files are saved to `ux0:data/VitaDrop/` on the Vita.

## What you need

- A PS Vita that can run homebrew (HENkaku/Ensō) with VitaShell installed
- A free GitHub account (GitHub builds the app for you)
- The Vita and the iPhone on the **same Wi-Fi network**

## Easiest: let GitHub build it

1. On github.com, click **+** (top right) > **New repository**. Name it `VitaDrop` and click **Create repository**.
2. On the new repo page, click **uploading an existing file**. Drag in everything from this folder **except** the `.github` folder (your computer may hide it anyway). Click **Commit changes**.
3. Add the build instructions: click **Add file > Create new file**. In the name box type `.github/workflows/build.yml` (typing `/` creates the folders). Paste in the contents of `.github/workflows/build.yml` from this folder and click **Commit changes**.
4. Click the **Actions** tab. A build named "Build VitaDrop" starts on its own and takes about a minute.
5. When it shows a green check, click it, scroll to **Artifacts**, and download **VitaDrop-vpk**. It's a zip with `VitaDrop.vpk` inside.

Then skip to **Step 3: Install it on the Vita**.

Every time you change a file on GitHub, it builds a new `.vpk` automatically. To publish a version anyone can download, go to **Releases > Create a new release**, make a tag like `v1.0`, and publish it. GitHub attaches `VitaDrop.vpk` to the release a minute later.

## Or build on your own computer

### Step 1: Install VitaSDK (one time)

VitaSDK is the free toolkit that turns this code into a Vita app.

1. Follow the install guide at https://vitasdk.org. It runs `vdpm` and installs all the libraries this app uses, including `vita2d`.
2. Add these two lines to your shell profile (`~/.zshrc` on Mac, `~/.bashrc` on Linux), then open a new terminal:
   ```
   export VITASDK=/usr/local/vitasdk
   export PATH=$VITASDK/bin:$PATH
   ```
3. Check that it worked: `arm-vita-eabi-gcc --version` should print a version number.

You also need `cmake` (Mac: `brew install cmake`, Ubuntu: `sudo apt install cmake`).

### Step 2: Build the app

From inside this folder, run:

```
mkdir build
cd build
cmake ..
make
```

The first build downloads a small QR code library, so it needs internet. When it finishes you'll have **`build/VitaDrop.vpk`**.

## Step 3: Install it on the Vita

1. On the Vita, open VitaShell and press **SELECT** to start its FTP server (or use USB mode).
2. Copy `VitaDrop.vpk` to `ux0:` on the Vita.
3. In VitaShell, select `VitaDrop.vpk` and press **X** to install it.

## Step 4: Use it

1. Open **VitaDrop** from the Vita home screen. It shows a QR code.
2. Open the iPhone **Camera**, point it at the Vita screen, and tap the link that pops up.
3. In Safari, tap **Choose files**, then pick photos, videos, or files from the Files app.
4. Wait until each file says **Saved**. Press **START** on the Vita to exit.

### Buttons

| Button | What it does |
|---|---|
| **CIRCLE** | Music on / off |
| **TRIANGLE** | Screen off (transfers keep going) |
| **START** | Exit |

### Music

VitaDrop plays a relaxed, original bossa-nova tune made by a small synthesizer built into the app, so there's no music file. Press **CIRCLE** to turn it off. To change the song, edit the note tables at the top of `src/music.c`.

### Screen-off mode (saves battery)

Press **TRIANGLE** to turn the Vita's screen off. VitaDrop keeps receiving files in the dark. Press any button to turn the screen back on.

While VitaDrop is open, the Vita won't go to sleep by itself, so close the app when you're done.

**Don't use the power button during a transfer.** It puts the Vita into real sleep, which pauses everything, including Wi-Fi, so any upload in progress stops. When you wake the Vita, VitaDrop reconnects by itself. Just choose the file again on your iPhone.

## Troubleshooting

- **"Waiting for Wi-Fi..." stays on screen**: connect the Vita in Settings > Network > Wi-Fi Settings, then reopen the app.
- **Safari can't open the page**: make sure the iPhone is on the same Wi-Fi network, not mobile data. Some guest or public networks block devices from talking to each other.
- **The same file name sent twice**: the second copy is saved as `name (1).ext`, so nothing gets overwritten.
- **iPhone photos arrive as .jpg instead of .heic**: that's normal. iOS often converts photos when Safari uploads them.
- **Upload failed**: the connection dropped or the memory card is full. A half-finished file is deleted automatically.

## How the code is organised

| File | What it does |
|---|---|
| `src/main.c` | Starts everything, connects to Wi-Fi, handles the buttons |
| `src/ui.c` | Draws the Vita screen (the colours for the theme are at the top) |
| `src/music.c` | The background music: a tiny synthesizer and the song's notes |
| `src/server.c` | The small web server that serves the upload page and saves files |
| `web/index.html` | The upload page your iPhone sees |
| `src/page_html.h` | `web/index.html` turned into C code. After editing the page, regenerate it with `python3 tools/embed_html.py` |
| `sce_sys/` | The app icon and LiveArea images |

## Security note

While VitaDrop is open, anyone on the same Wi-Fi network can upload files to the Vita (uploads only, so they can't read or delete anything). Close the app when you're done, and avoid using it on public Wi-Fi.
