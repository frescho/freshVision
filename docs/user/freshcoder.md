# freshCoder reference

freshCoder opens a **raw recording** made by freshVision (a composite `g8`
recording, or an RGB recording of an S-Video source, saved with the **FFV1**
or **Uncompressed** encoder) and turns it into a finished colour video. Because
the recording holds the card's raw samples, you can decode it again and again
with other settings: other colour, sharpness, palette, size or encoder. It
needs no capture card.

Open it from the menu, from freshVision's Recorder section (**Open in
freshCoder**), or with `freshcoder recording.mkv`.

## The window

The sections on the left, the preview on the right.

### Settings

Save the current settings under a name, **Load** or **Delete** them,
**Defaults** to start over. The last settings come back when freshCoder
starts (unless the command line sets options). Stored in
`~/.config/freshvision/freshcoder/`.

### Recording

**Open** / **Browse...** a recording. What freshCoder reads from it is shown:
size, sampling, frame rate, sound and the capture settings freshVision saved
in it. **standard**: PAL or NTSC (taken from the recording when it says).

**RGB inputs** (for RGB recordings): an S-Video recording says which inputs
carry brightness and colour; without that tag, choose them here
(**brightness input**, **colour input**), or decode one input as composite.

### Decoding

The same decoding options as in freshVision (see the [freshVision
reference](freshvision.md#decoding-composite-and-s-video)): line-pair
averaging, colour smoothing, edge colour reduction, two-colour text,
re-aligning rolling frames, removing clock stripes, brightness filter, PLL,
picture controls, pixel grid and palette. **Auto levels** sets brightness and
contrast from the frame shown: its darkest 0.5% to black, its brightest 0.1%
to 95% (what freshVision's auto-tune does at capture time, done digitally).

For "remove clock stripes", the encode first learns the stripes from the
first 10 seconds.

### Preview

Play, pause and step through the recording (**<** **>**), with the picture as
it will be encoded. **scale**, **linear filter** and the pixel aspect
(**automatic**, **1:1**, **4:3**) only change the preview.

### Output picture

**size**:

- **4:3, N x the lines**: a 4:3 picture N times as tall as the recording's
  lines (the default, 2x).
- **Native samples**: one output pixel per sample and line.
- **Custom size**: any **width x height**, or **720p** to **2160p**;
  **whole multiples of the lines only** keeps the lines crisp (no line
  scaled to 1.5 lines), padding the rest.
- **16:9 frame (padded)**: the 4:3 picture inside a 16:9 frame.
- **Integer (pixel-exact)**: with the pixel grid on, every machine pixel
  becomes an N x N block (**N (both ways)**, or **N across** and **N down**
  separately), or the largest that fits **into** a 720p-2160p frame. The text
  under it says whether the blocks are square and how far the result is from
  true 4:3 (unequal factors can correct it).

**scaler** (nearest, bilinear, bicubic, lanczos), **padding** colour, and
**crop** (in samples and lines, or machine pixels with the pixel grid; 0 size
= to the edge; **Reset crop**). The line at the bottom shows the decoded and
the output size.

### Encoding

- **video**: the encoders that work on this computer: x264, x265, NVENC
  (H.264/HEVC), SVT-AV1, ProRes, FFV1 (lossless).
- **quality** (lower = better; the lossless encoders have none) and
  **preset** (the encoder's speed/size trade-off).
- **4:4:4 colour (no colour subsampling)**: keeps full colour resolution
  (sharper colour edges, larger files; not every player supports it).
- **sound**: auto (PCM for FFV1/ProRes, FLAC otherwise), PCM, FLAC, AAC,
  Opus or none.
- **Save as...** chooses the output file; **Encode** starts, with progress and
  **Cancel**; **Open folder** when it's done.

The output keeps the recording's own frame timing (a C64 runs at about
50.12 frames per second, not exactly 50).

## Batch (command line)

```sh
freshcoder rec.mkv --encode --out out.mkv [options]
```

| Option | Does |
|---|---|
| `--codec ID` | encoder: `libx264` (default), `libx265`, `h264_nvenc`, `hevc_nvenc`, `libsvtav1`, `prores_ks`, `ffv1`; `--list-codecs` shows which work here |
| `--quality N`, `--preset P` | the encoder's quality (lower = better) and preset |
| `--chroma 444` or `420` | colour resolution (default 4:4:4 where the encoder can) |
| `--audio auto\|pcm\|flac\|aac\|opus\|none` | sound |
| `--size 43xN`, `native`, `WxH`, `720p`...`2160p` | output size (default `43x2`); `--integer` keeps whole line multiples |
| `--pixel-exact N`, `NXxNY` or `720p`...`2160p` | machine pixels as N x N blocks (with the pixel grid) |
| `--scaler nearest\|bilinear\|bicubic\|lanczos` | scaler (default bicubic) |
| `--crop X,Y,W,H`, `--pad RRGGBB` | crop, padding colour |
| `--standard auto\|pal\|ntsc` | colour standard |
| `--svideo auto\|L\|L,C` | RGB recordings: S-Video inputs, or one input as composite |
| `--luma notch\|box\|off`, `--no-average`, `--no-pll`, `--pll-speed F` | decoding |
| `--remove-stripes`, `--relock`, `--soft-text` | clock stripes, rolling frames, two-colour text |
| `--colour-smoothing N`, `--edge-colour F` | colour smoothing, edge colour reduction |
| `--brightness F --contrast F --saturation F --hue DEG --gamma F` | picture |
| `--grid`, `--snap`, `--c64-cells`, `--tms-cells` | pixel grid, palette snapping, chip colour rules |
| `--profile N\|NAME`, `--palette machine\|learned\|FILE`, `--core F`, `--colours N` | machine profile and palette |
| `--settings NAME` | use settings saved in the window (later options override them) |
| `--info` | print what the recording says about itself |
| `--version` | print the version |

Example: a C64 recording to 1080p H.264 with the machine's pixels and palette:

```sh
freshcoder c64.mkv --encode --out c64.mkv --profile 1 --snap --pixel-exact 1080p
```
