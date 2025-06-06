---
title: Linux
---


## Installing Grabber on Linux

### Via the AppImage
1. Go to the latest release [here](https://github.com/Bionus/imgbrd-grabber/releases/latest)
2. Download the `.AppImage` file
3. Make it executable using the command:
    ```bash
    chmod +x Grabber_*-x86_64.AppImage
    ```
4. Double-click on the AppImage file to start Grabber

### Via the pre-built binary archive
1. Go to the latest release [here](https://github.com/Bionus/imgbrd-grabber/releases/latest)
2. Download the `.tar.gz` file
3. Extract it anywhere using the command:
    ```bash
    tar -xzf Grabber_*.tar.gz
    ```
4. Launch the `Grabber` executable to start Grabber

!!! info

    Grabber requires the Qt6 libraries to run. If you see a "error while loading shared libraries" error, make sure you have the Qt6 packages installed.

### Via Flathub
1. Setup Flatpak and Flathub following the instructions [here](https://flathub.org/setup)
2. Install Grabber using the command:
    ```bash
    flatpak install flathub org.bionus.Grabber
    ```       
3. Run it using the command:
    ```bash
    flatpak run org.bionus.Grabber
    ```

### Via the AUR (Arch Linux)
Grabber has packages on the AUR for Arch Linux:

* [imgbrd-grabber-bin](https://aur.archlinux.org/packages/imgbrd-grabber-bin) which pulls the latest `tar.gz` release.
* [imgbrd-grabber-appimage](https://aur.archlinux.org/packages/imgbrd-grabber-appimage) which pulls the latest `appimage` release.
* [imgbrd-grabber](https://aur.archlinux.org/packages/imgbrd-grabber) which builds the package from source.

You can install Grabber by running this command, substituting the package you want to install:
```bash
paru -Sy imgbrd-grabber-bin
```
