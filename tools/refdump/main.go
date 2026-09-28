// refdump: run a ROM in gopher2600 and write raw TIA colour frames.
// usage: refdump rom.bin firstFrame numFrames outprefix
// Each output file holds lines x 160 bytes (TIA colour value, 0 in VBLANK)
// preceded by a text header "lines\n".
package main

import (
	"fmt"
	"os"
	"strconv"

	"github.com/jetsetilly/gopher2600/cartridgeloader"
	"github.com/jetsetilly/gopher2600/debugger/govern"
	"github.com/jetsetilly/gopher2600/environment"
	"github.com/jetsetilly/gopher2600/hardware"
	"github.com/jetsetilly/gopher2600/hardware/television"
	"github.com/jetsetilly/gopher2600/hardware/television/frameinfo"
	"github.com/jetsetilly/gopher2600/hardware/television/signal"
	"github.com/jetsetilly/gopher2600/hardware/television/specification"
	"github.com/jetsetilly/gopher2600/setup"
)

type dumper struct {
	frame  int
	first  int
	last   int
	prefix string
	buf    []byte
	maxIdx int
}

func (d *dumper) NewFrame(fi frameinfo.Current) error {
	if d.frame >= d.first && d.frame <= d.last && d.maxIdx > 0 {
		lines := d.maxIdx/specification.ClksScanline + 1
		f, err := os.Create(fmt.Sprintf("%s%04d.raw", d.prefix, d.frame))
		if err != nil {
			return err
		}
		fmt.Fprintf(f, "%d\n", lines)
		f.Write(d.buf[:lines*160])
		f.Close()
	}
	d.frame++
	for i := range d.buf {
		d.buf[i] = 0
	}
	d.maxIdx = 0
	return nil
}
func (d *dumper) NewScanline(scanline int) error { return nil }
func (d *dumper) SetPixels(sig []signal.SignalAttributes, last int) error {
	for i := 0; i <= last && i < len(sig); i++ {
		s := sig[i]
		if s.Index == signal.NoSignal {
			continue
		}
		line := s.Index / specification.ClksScanline
		clk := s.Index%specification.ClksScanline - specification.ClksHBlank
		if clk < 0 || line >= specification.AbsoluteMaxScanlines {
			continue
		}
		c := byte(s.Color)
		if s.VBlank || s.Color == signal.ZeroBlack {
			c = 0
		}
		d.buf[line*160+clk] = c
		if s.Index > d.maxIdx {
			d.maxIdx = s.Index
		}
	}
	return nil
}
func (d *dumper) Reset()              {}
func (d *dumper) EndRendering() error { return nil }

func main() {
	if len(os.Args) < 5 {
		fmt.Fprintln(os.Stderr, "usage: refdump rom.bin firstFrame numFrames outprefix")
		os.Exit(2)
	}
	first, _ := strconv.Atoi(os.Args[2])
	num, _ := strconv.Atoi(os.Args[3])
	tv, err := television.NewTelevision("AUTO")
	if err != nil {
		panic(err)
	}
	defer tv.End()
	tv.SetFPSLimit(false)
	d := &dumper{first: first, last: first + num - 1, prefix: os.Args[4],
		buf: make([]byte, specification.AbsoluteMaxScanlines*160)}
	tv.AddPixelRenderer(d)
	vcs, err := hardware.NewVCS(environment.MainEmulation, tv, nil, nil)
	if err != nil {
		panic(err)
	}
	vcs.Env.Normalise()
	cl, err := cartridgeloader.NewLoaderFromFilename(os.Args[1], "AUTO", "AUTO", nil, tv)
	if err != nil {
		panic(err)
	}
	defer cl.Close()
	if err := setup.AttachCartridge(vcs, cl, nil); err != nil {
		panic(err)
	}
	err = vcs.RunForFrameCount(first+num+1, func() (govern.State, error) { return govern.Running, nil })
	if err != nil {
		panic(err)
	}
}
