# Quick Save and Reload

ASI mod for `CrimsonDesert.exe` that adds a native-style quick save slot and hotkeys for quick save / quick load.

## Runtime Files

- `QuickSaveAndReload.asi`
- `QuickSaveAndReload.ini`
- `QuickSaveAndReload.log` (only when logging is enabled)

## What It Does

- reserves slot `108` as the dedicated quick save slot
- shows that slot as `Quick Save` at the top of the load screen
- hides the reserved quick slot from the normal save screen
- supports quick save and quick load without requiring a prior manual save
- uses the native quick-load confirmation modal by default
- can show a native success toast after quick save

## What It Hooks

Current implementation resolves and hooks the native save/load and UI paths via AOB:

- `DirectLocalSave`
- `SaveServiceDriver`
- `ServiceChildPoll`
- `InGameMenuLoadCore`
- `BuildVisibleMap`
- `LoadSelectedRefresh`
- `LoadModalHandler`
- `RenderSlotRow`
- `SetControlText`

Behavior:
- quick save writes to reserved slot `108`
- quick load reads from reserved slot `108`
- the top row in the load screen is rendered as `Quick Save`
- the reserved slot is hidden from the normal save list
- quick save / quick load are blocked during unsafe transition states

## Config

`QuickSaveAndReload.ini`

```ini
[General]
EnableMod=1
LogEnabled=0
ToastNotification=1
QuickLoadConfirmation=1
HotkeyQuickSave=F5
HotkeyQuickLoad=F6
_HotkeyOptions=F1-F12, INSERT, DELETE, HOME, END, PGUP, PGDN, or single letter A-Z

[Hotkeys]
ControllerHotkeyQuickSave=lb+a
ControllerHotkeyQuickLoad=lb+b
_ControllerHotkeyOptions=Use dpad_up/down/left/right + a/b/x/y/lb/rb/start/back
```

Notes:
- `EnableMod=1`: enables the mod.
- `LogEnabled=1`: writes startup and runtime diagnostics.
- `ToastNotification=1`: shows the native quick save success toast.
- `QuickLoadConfirmation=1`: shows the native confirmation modal before quick load.
- `HotkeyQuickSave`: keyboard quick save hotkey.
- `HotkeyQuickLoad`: keyboard quick load hotkey.
- `ControllerHotkeyQuickSave`: controller combo for quick save.
- `ControllerHotkeyQuickLoad`: controller combo for quick load.

## Build

- Solution: `Quicksaveandreload.slnx`
- Project: `Quicksaveandreload/Quicksaveandreload.vcxproj`
- Recommended: `Release | x64`

Compiler output is `Quicksaveandreload.dll`.  
Deployment runtime name is `QuickSaveAndReload.asi`.
