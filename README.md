# QuickSaveAndReload

Quick save and quick load mod for Crimson Desert.

Version: 1.4

## Install

Copy `QuickSaveAndReload.asi` next to `CrimsonDesert.exe`.

Optional config file: `QuickSaveAndReload.ini`

```ini
[General]
EnableMod=1
LogEnabled=0
ToastNotification=1
QuickLoadConfirmation=1
HotkeyQuickSave=F5
HotkeyQuickLoad=F6

[Hotkeys]
ControllerHotkeyQuickSave=lb+a
ControllerHotkeyQuickLoad=lb+y

[SaveRuntime]
QuickSaveSlotCount=1
```

`QuickSaveSlotCount` supports `1` to `8`.

## Build

Open `QuickSaveAndReload.slnx` and build `Release | x64`.

Output: `QuickSaveAndReload.asi`
