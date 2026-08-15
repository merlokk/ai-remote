# Generates the boot splash of CLAUDE.md 10.8 - white katakana, Matrix-fashion -
# straight into the raw format the panel is fed with, and writes it to
# `spiffs_image/splash.bin`.
#
# Run it with **Windows PowerShell 5.1** (`powershell.exe`, not `pwsh`): the
# rasteriser is `System.Drawing`, which is in the box there and a NuGet package
# on PowerShell 7. That is the whole reason this is not a Python script -
# rendering a glyph needs a font engine, and every route to one on this machine
# meant a new dependency. GDI+ is already installed on every Windows box, so
# root section 1's list is untouched.
#
#   powershell.exe -ExecutionPolicy Bypass -File tools\make-splash.ps1
#
# The output is **raw RGB565, big-endian, no header** - 480*480*2 = 460800
# bytes, which is what `display::BlitRaw` streams to the panel in strips.
# Big-endian because that is the byte order the CO5300 wants; the same swap
# LVGL is told to do with `flags.swap_bytes` is done here, once, at build time
# rather than 230400 times at boot. Getting it backwards does not fail - it
# produces plausible wrong colours, which is the failure worth naming.
#
# It is deterministic: a fixed seed, so re-running it produces the same file
# and a rebuild does not show up as a diff of 460 KB.

param(
    [int]$Width = 480,
    [int]$Height = 480,
    [int]$Seed = 20260816,
    [string]$Out = (Join-Path $PSScriptRoot "..\spiffs_image\splash.bin")
)

Add-Type -AssemblyName System.Drawing

# **The pixel loop is C#, and it has to be.** Written as PowerShell it is
# 230400 iterations of an interpreted language and takes minutes - measured,
# not feared: the first version of this script had to be killed. Everything
# above it runs once per glyph, a few hundred times, and stays PowerShell where
# it is readable.
Add-Type -TypeDefinition @"
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;

public static class Rgb565 {
    // Big-endian RGB565, no header - the byte order the CO5300 wants.
    public static byte[] FromBitmap(Bitmap bitmap) {
        int width = bitmap.Width, height = bitmap.Height;
        Rectangle rect = new Rectangle(0, 0, width, height);
        BitmapData data = bitmap.LockBits(rect, ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
        byte[] argb = new byte[data.Stride * height];
        Marshal.Copy(data.Scan0, argb, 0, argb.Length);
        bitmap.UnlockBits(data);

        byte[] raw = new byte[width * height * 2];
        int o = 0;
        for (int y = 0; y < height; y++) {
            int line = y * data.Stride;
            for (int x = 0; x < width; x++) {
                // BGRA in memory, which is what Format32bppArgb means on a
                // little-endian machine.
                int b = argb[line + x * 4];
                int g = argb[line + x * 4 + 1];
                int r = argb[line + x * 4 + 2];
                int v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
                raw[o++] = (byte)((v >> 8) & 0xFF);
                raw[o++] = (byte)(v & 0xFF);
            }
        }
        return raw;
    }
}
"@ -ReferencedAssemblies System.Drawing

# The glyph cell. 20 px gives 24 columns of 24 rows on a 480 px panel, which is
# dense enough to read as rain rather than as a list.
$cell = 20
$fontSize = 15
$columns = [int]($Width / $cell)
$rows = [int]($Height / $cell)

# Full-width katakana, which is what the film's columns are (its glyphs are
# mirrored; these are not, and nobody has ever noticed). A handful of digits
# because the original has them too.
$glyphs = @()
foreach ($code in 0x30A1..0x30FA) { $glyphs += [char]$code }
foreach ($code in 0x30..0x39) { $glyphs += [char]$code }

$random = New-Object System.Random($Seed)

$bitmap = New-Object System.Drawing.Bitmap($Width, $Height, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
$graphics = [System.Drawing.Graphics]::FromImage($bitmap)
$graphics.Clear([System.Drawing.Color]::Black)
$graphics.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAliasGridFit

# MS Gothic: monospaced, ships with Windows, and has the whole katakana block.
$font = New-Object System.Drawing.Font("MS Gothic", $fontSize, [System.Drawing.FontStyle]::Regular, [System.Drawing.GraphicsUnit]::Pixel)

for ($column = 0; $column -lt $columns; $column++) {
  # One or two streaks per column. One alone leaves whole quadrants black -
  # visible in the first render as an empty bottom-right corner, which reads as
  # a bug rather than as rain. Two is enough to cover without becoming a wall
  # of text: the gaps are the effect.
  $streaks = if ($random.Next(0, 100) -lt 55) { 2 } else { 1 }
  for ($streak = 0; $streak -lt $streaks; $streak++) {
    # A head somewhere down the screen and a tail above it - the head is the
    # bottom of the streak, because the rain falls.
    $head = $random.Next(-4, $rows + 6)
    $length = $random.Next(5, 18)

    for ($i = 0; $i -lt $length; $i++) {
        $row = $head - $i
        if ($row -lt 0 -or $row -ge $rows) { continue }

        # The head is white and the tail falls away. Not linear: the first two
        # or three glyphs carry the streak and the rest is texture.
        if ($i -eq 0) {
            $level = 255
        } else {
            $level = [int](210 * [Math]::Pow(0.80, $i)) + 12
        }
        if ($level -gt 255) { $level = 255 }
        if ($level -lt 0) { $level = 0 }

        $colour = [System.Drawing.Color]::FromArgb(255, $level, $level, $level)
        $brush = New-Object System.Drawing.SolidBrush($colour)
        $glyph = $glyphs[$random.Next(0, $glyphs.Length)]
        $graphics.DrawString([string]$glyph, $font, $brush, ($column * $cell), ($row * $cell))
        $brush.Dispose()
    }
  }
}

$graphics.Dispose()

# --- Bitmap to raw RGB565 big-endian -------------------------------------
$raw = [Rgb565]::FromBitmap($bitmap)
$bitmap.Dispose()

$resolved = [System.IO.Path]::GetFullPath($Out)
[System.IO.File]::WriteAllBytes($resolved, $raw)
Write-Host ("{0}: {1}x{2}, {3} bytes (raw rgb565be)" -f $resolved, $Width, $Height, $raw.Length)
