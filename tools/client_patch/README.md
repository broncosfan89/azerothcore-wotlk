## Item.dbc client patch (WotLK 3.3.5)

This folder contains a patched `Item.dbc`:

- `DBFilesClient/Item.dbc`

It includes custom item rows for:

- `59001` .. `60509` (slot `01..09` for Mythic levels `0..15`)

cloned from source rows:

- `35570` .. `35578`

### Build/regenerate

From repo root:

```powershell
python tools/patch_item_dbc.py `
  --input build/bin/Release/Data/dbc/Item.dbc `
  --output tools/client_patch/DBFilesClient/Item.dbc `
  --in-place
```

One-command workflow (patch DBC + build MPQ):

```powershell
powershell -ExecutionPolicy Bypass -File tools/build_item_patch.ps1 -PatchInputInPlace
```

### Package for client

Use an MPQ editor to create a patch file (for example `patch-Z.MPQ`) and add:

- internal MPQ path: `DBFilesClient/Item.dbc`
- source file: `tools/client_patch/DBFilesClient/Item.dbc`

Place `patch-Z.MPQ` in your WoW 3.3.5 `Data/<locale>/` folder (for example `Data/enUS/`).

### After patching client

1. Fully close WoW client.
2. Clear cache (`Cache/` and WDB item cache files).
3. Start client and test custom items in the `590xx` through `605xx` range.
