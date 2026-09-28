# refdump

Runs a ROM in [gopher2600](https://github.com/jetsetilly/gopher2600) and writes
raw frames (`<lines>\n` + lines × 160 TIA colour bytes, 0 in VBLANK) for
`tools/refcompare.py` / `tools/refcheck.sh`.

```sh
# go.mod expects a gopher2600 checkout next to this repository; adjust the
# replace directive otherwise, and copy gopher2600's go.sum here
cp /path/to/gopher2600/go.sum .
GOFLAGS=-mod=mod go build -o refdump .
./refdump rom.bin <first frame> <count> out/prefix_
```

gopher2600 runs with normalised preferences (no random start state), which
`a26host -clean` mirrors.
