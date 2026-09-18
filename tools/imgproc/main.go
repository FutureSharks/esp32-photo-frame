// imgproc turns ordinary photographs into frames the Inky Impression 13.3"
// (EL133UF1 / Spectra 6) can display directly.
//
// Everything expensive happens here rather than on the device: scaling,
// cropping, reduction to the panel's six colours, dithering, and the split
// between the two controllers. The panel is hung in portrait, so images are
// composed directly in the controllers' portrait scan order - no rotation is
// needed. What lands in images/art/processed/ is the exact byte stream the
// firmware pushes over SPI, so the ESP32 needs no image decoding and no
// framebuffer - it copies bytes from the socket to the panel.
//
// Usage:
//
//	go run ./tools/imgproc                      # images/art/*.jpg -> images/art/processed/
//	go run ./tools/imgproc -saturation 0.7      # more vivid, less faithful
//	go run ./tools/imgproc -fit contain         # letterbox instead of cropping
//
// Output format, per file (960,000 bytes):
//
//	bytes      0..479,999  CS_M half: 1600 rows of 300 bytes, portrait columns 0..599
//	bytes 480,000..959,999  CS_S half: same, portrait columns 600..1199
//
// Two pixels per byte, high nibble first, using the panel's colour codes.
package main

import (
	"bufio"
	"flag"
	"fmt"
	"image"
	_ "image/jpeg"
	_ "image/png"
	"math"
	"os"
	"path/filepath"
	"sort"
	"strings"
)

// Panel geometry. The glass is 1600x1200 landscape, but the controllers scan
// it as 1200x1600 portrait, which is also how the frame hangs.
const (
	portraitW = 1200
	portraitH = 1600

	rowBytes    = 300 // one portrait row for one controller (600 px / 2)
	halfBytes   = rowBytes * portraitH
	streamBytes = halfBytes * 2
)

// The panel's 4-bit colour codes. Note 0x4 is not a colour.
var panelCodes = [6]byte{0x0, 0x1, 0x2, 0x3, 0x5, 0x6}

// Pimoroni's two reference palettes. The saturated set is roughly what the ink
// actually produces; the desaturated set is the idealised primaries. Blending
// between them trades colour accuracy for punch - see -saturation.
var (
	saturatedPalette = [6][3]float64{
		{0, 0, 0},       // black
		{161, 164, 165}, // white
		{208, 190, 71},  // yellow
		{156, 72, 75},   // red
		{61, 59, 94},    // blue
		{58, 91, 70},    // green
	}
	desaturatedPalette = [6][3]float64{
		{0, 0, 0},
		{255, 255, 255},
		{255, 255, 0},
		{255, 0, 0},
		{0, 0, 255},
		{0, 255, 0},
	}
)

func blendPalette(saturation float64) [6][3]float64 {
	var p [6][3]float64
	for i := 0; i < 6; i++ {
		for c := 0; c < 3; c++ {
			p[i][c] = saturatedPalette[i][c]*saturation +
				desaturatedPalette[i][c]*(1-saturation)
		}
	}
	return p
}

func main() {
	in := flag.String("in", "images/art", "directory of source images")
	out := flag.String("out", "images/art/processed", "directory for packed frames")
	saturation := flag.Float64("saturation", 0.5,
		"0 = idealised primaries, 1 = measured ink colours")
	fit := flag.String("fit", "cover",
		"cover (fill the panel, cropping overflow) or contain (letterbox on white)")
	force := flag.Bool("force", false, "repack images whose .bin is already up to date")
	flag.Parse()

	if *fit != "cover" && *fit != "contain" {
		fatalf("-fit must be cover or contain, got %q", *fit)
	}
	if *saturation < 0 || *saturation > 1 {
		fatalf("-saturation must be between 0 and 1, got %v", *saturation)
	}

	sources, err := findSources(*in)
	if err != nil {
		fatalf("scanning %s: %v", *in, err)
	}
	if len(sources) == 0 {
		fatalf("no .jpg/.jpeg/.png files in %s", *in)
	}

	if err := os.MkdirAll(*out, 0o755); err != nil {
		fatalf("creating %s: %v", *out, err)
	}

	palette := blendPalette(*saturation)
	var packed []string

	for _, src := range sources {
		name := strings.TrimSuffix(filepath.Base(src), filepath.Ext(src)) + ".bin"
		dst := filepath.Join(*out, name)

		if !*force && upToDate(src, dst) {
			fmt.Printf("  %-40s up to date\n", filepath.Base(src))
			packed = append(packed, name)
			continue
		}

		if err := process(src, dst, palette, *fit); err != nil {
			// One bad file should not stop the batch; it just will not appear
			// in the manifest, so the device never asks for it.
			fmt.Fprintf(os.Stderr, "  %-40s FAILED: %v\n", filepath.Base(src), err)
			continue
		}
		fmt.Printf("  %-40s -> %s\n", filepath.Base(src), name)
		packed = append(packed, name)
	}

	if len(packed) == 0 {
		fatalf("nothing packed successfully")
	}

	sort.Strings(packed)
	manifest := filepath.Join(*out, "manifest.txt")
	if err := writeManifest(manifest, packed); err != nil {
		fatalf("writing %s: %v", manifest, err)
	}
	fmt.Printf("\n%d image(s), %s\n", len(packed), manifest)
}

func findSources(dir string) ([]string, error) {
	entries, err := os.ReadDir(dir)
	if err != nil {
		return nil, err
	}
	var out []string
	for _, e := range entries {
		if e.IsDir() {
			continue
		}
		switch strings.ToLower(filepath.Ext(e.Name())) {
		case ".jpg", ".jpeg", ".png":
			out = append(out, filepath.Join(dir, e.Name()))
		}
	}
	sort.Strings(out)
	return out, nil
}

// upToDate reports whether dst exists, is the right size, and is newer than src.
func upToDate(src, dst string) bool {
	di, err := os.Stat(dst)
	if err != nil || di.Size() != streamBytes {
		return false
	}
	si, err := os.Stat(src)
	if err != nil {
		return false
	}
	return di.ModTime().After(si.ModTime())
}

func process(src, dst string, palette [6][3]float64, fit string) error {
	f, err := os.Open(src)
	if err != nil {
		return err
	}
	defer f.Close()

	img, _, err := image.Decode(bufio.NewReader(f))
	if err != nil {
		return fmt.Errorf("decoding: %w", err)
	}

	// Scale to the panel, then reduce to six colours. Dithering happens on the
	// final-size image so the error diffuses across pixels the panel actually
	// has, rather than being smeared by a later resize.
	rgb := resample(img, portraitW, portraitH, fit)
	indices := dither(rgb, portraitW, portraitH, palette)

	return writeFrame(dst, indices)
}

// resample scales src to exactly w x h using bilinear interpolation.
//
// "cover" scales to fill and centre-crops the overflow; "contain" scales to fit
// and pads with white, which on this panel is a real ink colour rather than an
// absence of one.
func resample(src image.Image, w, h int, fit string) []float64 {
	b := src.Bounds()
	sw, sh := b.Dx(), b.Dy()

	scale := math.Max(float64(w)/float64(sw), float64(h)/float64(sh))
	if fit == "contain" {
		scale = math.Min(float64(w)/float64(sw), float64(h)/float64(sh))
	}

	dw, dh := int(math.Round(float64(sw)*scale)), int(math.Round(float64(sh)*scale))
	offX, offY := (dw-w)/2, (dh-h)/2 // negative under "contain": the padding

	out := make([]float64, w*h*3)
	for y := 0; y < h; y++ {
		for x := 0; x < w; x++ {
			dx, dy := x+offX, y+offY
			i := (y*w + x) * 3

			if dx < 0 || dy < 0 || dx >= dw || dy >= dh {
				out[i], out[i+1], out[i+2] = 255, 255, 255 // letterbox
				continue
			}

			// Map back into source coordinates, sampling at pixel centres.
			fx := (float64(dx)+0.5)/scale - 0.5
			fy := (float64(dy)+0.5)/scale - 0.5
			r, g, bl := bilinear(src, b, sw, sh, fx, fy)
			out[i], out[i+1], out[i+2] = r, g, bl
		}
	}
	return out
}

func bilinear(src image.Image, b image.Rectangle, sw, sh int, fx, fy float64) (float64, float64, float64) {
	x0, y0 := int(math.Floor(fx)), int(math.Floor(fy))
	tx, ty := fx-float64(x0), fy-float64(y0)

	at := func(x, y int) (float64, float64, float64) {
		x = clamp(x, 0, sw-1)
		y = clamp(y, 0, sh-1)
		// Go returns 16-bit premultiplied values; scale to 0..255.
		r, g, bl, _ := src.At(b.Min.X+x, b.Min.Y+y).RGBA()
		return float64(r) / 257, float64(g) / 257, float64(bl) / 257
	}

	r00, g00, b00 := at(x0, y0)
	r10, g10, b10 := at(x0+1, y0)
	r01, g01, b01 := at(x0, y0+1)
	r11, g11, b11 := at(x0+1, y0+1)

	lerp := func(a, b, t float64) float64 { return a + (b-a)*t }
	r := lerp(lerp(r00, r10, tx), lerp(r01, r11, tx), ty)
	g := lerp(lerp(g00, g10, tx), lerp(g01, g11, tx), ty)
	bl := lerp(lerp(b00, b10, tx), lerp(b01, b11, tx), ty)
	return r, g, bl
}

// dither reduces the image to palette indices using Floyd-Steinberg error
// diffusion, which is what makes six colours look like a photograph.
func dither(rgb []float64, w, h int, palette [6][3]float64) []uint8 {
	indices := make([]uint8, w*h)

	for y := 0; y < h; y++ {
		for x := 0; x < w; x++ {
			i := (y*w + x) * 3
			r, g, b := rgb[i], rgb[i+1], rgb[i+2]

			best, bestDist := 0, math.MaxFloat64
			for p := 0; p < 6; p++ {
				dr := r - palette[p][0]
				dg := g - palette[p][1]
				db := b - palette[p][2]
				// Weighted for perceived brightness; plain Euclidean distance
				// in RGB tends to pick green far too often.
				d := 0.299*dr*dr + 0.587*dg*dg + 0.114*db*db
				if d < bestDist {
					best, bestDist = p, d
				}
			}
			indices[y*w+x] = uint8(best)

			er := r - palette[best][0]
			eg := g - palette[best][1]
			eb := b - palette[best][2]

			spread := func(dx, dy int, factor float64) {
				nx, ny := x+dx, y+dy
				if nx < 0 || nx >= w || ny < 0 || ny >= h {
					return
				}
				j := (ny*w + nx) * 3
				rgb[j] += er * factor
				rgb[j+1] += eg * factor
				rgb[j+2] += eb * factor
			}
			spread(1, 0, 7.0/16)
			spread(-1, 1, 3.0/16)
			spread(0, 1, 5.0/16)
			spread(1, 1, 1.0/16)
		}
	}
	return indices
}

// writeFrame splits the portrait image at column 600, one half per controller,
// and packs two pixels per byte. The image is already in scan order, so this is
// a straight walk across it - which is what lets the device stream from a
// socket to SPI without a framebuffer.
func writeFrame(dst string, indices []uint8) error {
	buf := make([]byte, 0, streamBytes)

	for half := 0; half < 2; half++ {
		base := half * 600
		for y := 0; y < portraitH; y++ {
			for k := 0; k < rowBytes; k++ {
				xe := base + 2*k // even portrait column -> high nibble
				xo := xe + 1
				hi := panelCodes[indices[y*portraitW+xe]]
				lo := panelCodes[indices[y*portraitW+xo]]
				buf = append(buf, hi<<4|lo)
			}
		}
	}

	if len(buf) != streamBytes {
		return fmt.Errorf("packed %d bytes, expected %d", len(buf), streamBytes)
	}
	return os.WriteFile(dst, buf, 0o644)
}

func writeManifest(path string, names []string) error {
	var sb strings.Builder
	sb.WriteString("# Generated by tools/imgproc - one packed frame per line.\n")
	for _, n := range names {
		sb.WriteString(n)
		sb.WriteByte('\n')
	}
	return os.WriteFile(path, []byte(sb.String()), 0o644)
}

func clamp(v, lo, hi int) int {
	if v < lo {
		return lo
	}
	if v > hi {
		return hi
	}
	return v
}

func fatalf(format string, args ...any) {
	fmt.Fprintf(os.Stderr, "imgproc: "+format+"\n", args...)
	os.Exit(1)
}
