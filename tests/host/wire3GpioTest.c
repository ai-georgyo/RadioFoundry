// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The three-wire bus on pin callbacks (drivers/bus/wire3_gpio) against models of two chips on its lines, each driven
// by its real driver: a SKY72310 synthesizer (16-bit words, taken when the select rises; SKY72310-11 data sheet
// 203639L) and an AK2365A IF receiver (16 clocks with an empty slot between the address and the data, and a read
// frame that turns the data line around; data sheet MS1591-E-03 pages 9 and 10). Both clock idle levels; the select
// around every clock edge; no clock edge with nothing selected
#include <stdio.h>
#include <string.h>
#include "bus/wire3_gpio/wire3_gpio.h"
#include "rf/sky72310/sky72310.h"
#include "rf/ak2365a/ak2365a.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

#define CS_SYNTH	0u
#define CS_IF		1u

// --- The lines ---

static struct {
	bool clk;
	bool cs[2];			// true = high = not selected
	bool host_drives, host_level;	// the MCU's data pin
	bool pull_up;			// its pull while it is an input
	uint32_t t;			// half periods waited
	uint32_t t_clk, t_data, t_cs;	// when each last moved
	uint32_t t_release;		// when the MCU last let the data line go
	int calls;			// callbacks made
	int clk_edges;
	int edges_deselected;		// the clock moved with no chip selected
	int data_moved_clk_high;	// the data line changed inside a frame while the clock was high
	int timing;			// a set-up, hold or pulse shorter than half a period
	int contention;			// the MCU and a chip drove the data line together
	int both_selected;
	int bad_ctx, bad_cs;
	int releases, samples;
} bus;

static int bus_ctx;			// its address is the context every callback must get

// --- SKY72310: bits shift in on rising edges; the last 16 load a register when the select rises ---

static struct {
	uint32_t sr;
	int clocks;
	uint16_t reg[16];
	uint16_t words[32];
	int n;				// words loaded
	int frames;
	uint32_t last_sr;		// the last frame, whatever its length
	int last_clocks;
} synth;

// --- AK2365A: R/W and A5..A0 on rising edges 1..7, an empty slot, D7..D0 on edges 9..16 (write) or out of the chip
// after the falling edges of clocks 8..15 (read) ---

static struct {
	int rises;
	uint8_t head;
	bool read;
	uint8_t addr, data;
	uint8_t reg[64];
	bool drives, level;
	int frames, writes, reads;
	int void_writes;		// the select rose before the 16th clock
	int slot_high;			// the empty slot must be low
	int undriven;			// a bit taken with nothing driving the line
	int hiz_late;			// read: the line was not released before the 8th rising edge
	int bad_release;		// read: the line was let go too early, or not half a period before the 7th falling edge
	int extra_clocks;
	int last_rises;
} ifc;

static void ifc_reset_regs(void)
{
	memset(ifc.reg, 0, sizeof(ifc.reg));
	ifc.reg[AK2365A_REG_CTRL1] = 0x41u;
	ifc.reg[AK2365A_REG_CTRL2] = 0x32u;
	ifc.reg[AK2365A_REG_CTRL4] = 0x01u;
	ifc.reg[AK2365A_REG_CTRL5] = 0x80u;
}

static bool line(void)
{
	if (ifc.drives) {
		return ifc.level;
	}
	return bus.host_drives ? bus.host_level : bus.pull_up;
}

static void rig(bool clk_idle_high)
{
	memset(&bus, 0, sizeof(bus));
	memset(&synth, 0, sizeof(synth));
	memset(&ifc, 0, sizeof(ifc));
	bus.cs[CS_SYNTH] = true;
	bus.cs[CS_IF] = true;
	bus.clk = clk_idle_high;	// the board parked it
	ifc_reset_regs();
}

static void synth_rise(void)
{
	synth.sr = (synth.sr << 1) | (line() ? 1u : 0u);
	synth.clocks++;
}

static void ifc_rise(void)
{
	bool bit = line();
	ifc.rises++;
	if (ifc.rises <= 7) {
		if (!bus.host_drives) {
			ifc.undriven++;
		}
		ifc.head = (uint8_t)((ifc.head << 1) | (bit ? 1u : 0u));
		if (ifc.rises == 7) {
			ifc.read = (ifc.head & 0x40u) != 0u;
			ifc.addr = ifc.head & 0x3Fu;
		}
		return;
	}
	if (ifc.read) {
		if (ifc.rises == 8 && bus.host_drives) {
			ifc.hiz_late++;
		}
		return;
	}
	if (!bus.host_drives) {
		ifc.undriven++;
	}
	if (ifc.rises == 8) {
		if (bit) {
			ifc.slot_high++;
		}
	} else if (ifc.rises <= 16) {
		ifc.data = (uint8_t)((ifc.data << 1) | (bit ? 1u : 0u));
		if (ifc.rises == 16) {
			// The data is fixed on the 16th rising edge
			if (ifc.addr == AK2365A_REG_SRST) {
				if (ifc.data == AK2365A_SRST_VALUE) {
					ifc_reset_regs();
				}
			} else {
				ifc.reg[ifc.addr] = ifc.data;
			}
			ifc.writes++;
		}
	} else {
		ifc.extra_clocks++;
	}
}

static void ifc_fall(void)
{
	if (!ifc.read) {
		return;
	}
	// The chip may drive the line from the falling edge of clock 7 on (the data sheet calls the slot before D7
	// unstable): the MCU is off it by then, and has been for half a period
	if (ifc.rises == 7 && (bus.host_drives || bus.t - bus.t_release < 1u)) {
		ifc.bad_release++;
	}
	// The falling edge of clock k: the edge after the k-th rising one
	if (ifc.rises >= 8 && ifc.rises <= 15) {
		ifc.drives = true;
		ifc.level = ((ifc.reg[ifc.addr] >> (15 - ifc.rises)) & 1u) != 0u;
		if (bus.host_drives) {
			bus.contention++;
		}
	} else if (ifc.rises >= 16) {
		ifc.drives = false;
	}
}

static bool ctx_ok(void *ctx)
{
	bus.calls++;
	if (ctx != &bus_ctx) {
		bus.bad_ctx++;
		return false;
	}
	return true;
}

static void op_cs(void *ctx, uint8_t cs, bool level)
{
	if (!ctx_ok(ctx)) {
		return;
	}
	if (cs > CS_IF) {
		bus.bad_cs++;
		return;
	}
	if (level == bus.cs[cs]) {
		return;
	}
	// The select holds half a period after the last clock edge and stays high as long between frames
	if (bus.t - bus.t_clk < 1u && bus.clk_edges > 0) {
		bus.timing++;
	}
	if (!level && bus.t - bus.t_cs < 1u && synth.frames + ifc.frames > 0) {
		bus.timing++;
	}
	if (!level && !bus.cs[cs ^ 1u]) {
		bus.both_selected++;
	}
	bus.cs[cs] = level;
	bus.t_cs = bus.t;
	if (cs == CS_SYNTH) {
		if (!level) {
			synth.sr = 0;
			synth.clocks = 0;
			synth.frames++;
		} else {
			synth.last_sr = synth.sr;
			synth.last_clocks = synth.clocks;
			if (synth.clocks >= 16) {
				uint16_t w = (uint16_t)synth.sr;
				synth.reg[w >> 12] = w & 0x0FFFu;
				if (synth.n < 32) {
					synth.words[synth.n] = w;
				}
				synth.n++;
			}
		}
	} else {
		if (!level) {
			ifc.rises = 0;
			ifc.head = 0;
			ifc.data = 0;
			ifc.read = false;
			ifc.drives = false;
			ifc.frames++;
		} else {
			ifc.drives = false;
			ifc.last_rises = ifc.rises;
			if (ifc.read) {
				ifc.reads++;
			} else if (ifc.rises < 16) {
				ifc.void_writes++;
			}
		}
	}
}

static void op_clk(void *ctx, bool level)
{
	if (!ctx_ok(ctx) || level == bus.clk) {
		return;
	}
	bus.clk_edges++;
	if (bus.cs[CS_SYNTH] && bus.cs[CS_IF]) {
		bus.edges_deselected++;
	} else {
		// High and low times, the select before the first edge, the data before a rising edge
		if (bus.t - bus.t_clk < 1u || bus.t - bus.t_cs < 1u) {
			bus.timing++;
		}
		if (level && bus.host_drives && bus.t - bus.t_data < 1u) {
			bus.timing++;
		}
	}
	bus.clk = level;
	bus.t_clk = bus.t;
	if (!bus.cs[CS_SYNTH] && level) {
		synth_rise();
	}
	if (!bus.cs[CS_IF]) {
		if (level) {
			ifc_rise();
		} else {
			ifc_fall();
		}
	}
}

static void op_data(void *ctx, bool level)
{
	if (!ctx_ok(ctx)) {
		return;
	}
	if (!bus.host_drives || bus.host_level != level) {
		if (bus.clk && !(bus.cs[CS_SYNTH] && bus.cs[CS_IF])) {
			bus.data_moved_clk_high++;
		}
		bus.t_data = bus.t;
	}
	if (ifc.drives) {
		bus.contention++;
	}
	bus.host_drives = true;
	bus.host_level = level;
}

static void op_release(void *ctx, bool pull_up)
{
	if (!ctx_ok(ctx)) {
		return;
	}
	// A read's turn-round: the address' last bit is held half a period after its rising edge, and the line is let
	// go with the clock still high
	if (!bus.cs[CS_IF] && ifc.read && ifc.rises == 7 && (!bus.clk || bus.t - bus.t_clk < 1u)) {
		ifc.bad_release++;
	}
	bus.host_drives = false;
	bus.pull_up = pull_up;
	bus.t_release = bus.t;
	bus.releases++;
}

static bool op_in(void *ctx)
{
	if (!ctx_ok(ctx)) {
		return false;
	}
	// A chip's bit is there half a period after the falling edge
	if (bus.clk || bus.t - bus.t_clk < 1u) {
		bus.timing++;
	}
	if (bus.host_drives) {
		bus.contention++;	// reading its own level
	}
	bus.samples++;
	return line();
}

static void op_delay(void *ctx)
{
	if (ctx_ok(ctx)) {
		bus.t++;
	}
}

static const wire3_gpio_t gpio_low = { op_cs, op_clk, op_data, op_release, op_in, op_delay, false, &bus_ctx };
static const wire3_gpio_t gpio_high = { op_cs, op_clk, op_data, op_release, op_in, op_delay, true, &bus_ctx };

static const wire3_gpio_t *gpio_of(bool idle_high)
{
	return idle_high ? &gpio_high : &gpio_low;
}

static bool lines_clean(bool idle_high)
{
	return bus.edges_deselected == 0 && bus.data_moved_clk_high == 0 && bus.timing == 0 && bus.contention == 0 &&
	       bus.both_selected == 0 && bus.bad_ctx == 0 && bus.bad_cs == 0 && bus.clk == idle_high &&
	       bus.cs[CS_SYNTH] && bus.cs[CS_IF] && ifc.undriven == 0 && ifc.slot_high == 0 && ifc.hiz_late == 0 &&
	       ifc.extra_clocks == 0 && ifc.void_writes == 0 && ifc.bad_release == 0 &&
	       bus.t - bus.t_cs >= 1u;		// the select has been high half a period when a frame returns
}

// --- The synthesizer's driver on the bus ---

static const wire3_gpio_t *synth_gpio;

static bool synth_write(void *ctx, uint16_t word)
{
	(void)ctx;
	return wire3_gpio_write((void *)synth_gpio, CS_SYNTH, word, 16);
}

static void no_delay_us(uint32_t us)
{
	(void)us;
}

static const sky72310_bus_t synth_bus = { synth_write, NULL, no_delay_us, NULL };

static void test_synth(bool idle_high)
{
	rig(idle_high);
	synth_gpio = gpio_of(idle_high);
	sky72310_t dev;
	sky72310_attach(&dev, &synth_bus, 16800000u, 31);
	CHECK(sky72310_init(&dev));
	CHECK(synth.reg[SKY72310_REG_PD_GAIN] == 31);
	CHECK(synth.reg[SKY72310_REG_DELTA_SIGMA] == (SKY72310_DS_BASE | SKY72310_DS_SIGNED));

	int before = synth.n;
	CHECK(before > 0 && before + 4 <= 32);
	CHECK(sky72310_set_vco(&dev, 2, 446000000u));
	sky72310_ratio_t want;
	CHECK(sky72310_ratio(16800000u, 2, 446000000u, &want));
	CHECK(synth.n == before + 4);
	CHECK(synth.words[before] == sky72310_word(SKY72310_REG_REF_DIV, 1));
	CHECK(synth.reg[SKY72310_REG_DIVIDER] == want.n_reg);
	CHECK(synth.reg[SKY72310_REG_DIVIDEND_LSB] == (want.dividend & 0xFFu));
	CHECK(synth.reg[SKY72310_REG_DIVIDEND_MSB] == (want.dividend >> 8));
	CHECK(synth.words[before + 3] >> 12 == SKY72310_REG_DIVIDEND_MSB);	// the order the driver sent
	CHECK(synth.n == (int)dev.writes);
	CHECK(synth.frames == synth.n);
	CHECK(synth.last_clocks == 16);

	// The other chip on the lines saw none of it
	CHECK(ifc.frames == 0);
	CHECK(lines_clean(idle_high));
	// Nothing drives the data line anew after a frame: it keeps the last bit
	CHECK(bus.host_drives && bus.host_level == ((synth.words[synth.n - 1] & 1u) != 0u));
	CHECK(bus.releases == 0 && bus.samples == 0);
}

// Every word length, MSB first
static void test_lengths(bool idle_high)
{
	static const struct { uint32_t word; uint8_t bits; } cases[] = {
		{ 0x1u, 1 }, { 0x0u, 1 }, { 0x15u, 5 }, { 0xA5C3u, 16 }, { 0x800001u, 24 }, { 0x5A5A5Au, 24 },
		{ 0x80000001u, 32 }, { 0xDEADBEEFu, 32 },
	};
	rig(idle_high);
	for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		CHECK(wire3_gpio_write((void *)gpio_of(idle_high), CS_SYNTH, cases[i].word, cases[i].bits));
		CHECK(synth.last_clocks == cases[i].bits);
		CHECK(synth.last_sr == cases[i].word);
	}
	// Bits above the count are not sent
	CHECK(wire3_gpio_write((void *)gpio_of(idle_high), CS_SYNTH, 0xFFFFFF05u, 4));
	CHECK(synth.last_clocks == 4 && synth.last_sr == 0x5u);
	CHECK(lines_clean(idle_high));
}

// --- The IF receiver's driver on the bus ---

static void test_if_write(bool idle_high)
{
	static const ak2365a_cfg_t cfg = { 0xF1u, 0xE9u, 0x1Du, 0x02u, 0x15u, 0x8Au };
	rig(idle_high);
	const wire3_bus_t w3 = { wire3_gpio_write, (void *)gpio_of(idle_high) };
	ifc.reg[AK2365A_REG_CTRL3] = 0x03u;	// left from before: the software reset clears it first

	CHECK(ak2365a_setup(&w3, CS_IF, &cfg, true));
	CHECK(ifc.frames == AK2365A_SETUP_WORDS && ifc.writes == AK2365A_SETUP_WORDS);
	CHECK(ifc.last_rises == 16);
	CHECK(ifc.reg[AK2365A_REG_CTRL1] == 0xF1u);
	CHECK(ifc.reg[AK2365A_REG_CTRL2] == 0x1Du);
	CHECK(ifc.reg[AK2365A_REG_CTRL3] == 0x02u);
	CHECK(ifc.reg[AK2365A_REG_CTRL4] == 0x15u);
	CHECK(ifc.reg[AK2365A_REG_CTRL5] == 0x8Au);
	CHECK(ak2365a_setup(&w3, CS_IF, &cfg, false));
	CHECK(ifc.reg[AK2365A_REG_CTRL1] == 0xE9u);
	CHECK(ifc.reads == 0);
	CHECK(synth.frames == 0);
	CHECK(lines_clean(idle_high));

	// One clock short: the chip drops the word (what the 16 in the driver's frame is for)
	CHECK(wire3_gpio_write((void *)gpio_of(idle_high), CS_IF, ak2365a_word(AK2365A_REG_CTRL3, 0x01u) >> 1, 15));
	CHECK(ifc.void_writes == 1);
	CHECK(ifc.reg[AK2365A_REG_CTRL3] == 0x02u);
}

// One register read: R/W 1 and the address, the line released, then the samples. A clock that idles low: 10 samples
// make the frame's 16 clocks (the slot, D7..D0, and one after the chip let go). One that idles high returns high
// before the select rises, so 9 samples make them
static bool if_read(bool idle_high, uint8_t addr, bool pull_up, uint8_t *value, bool *slot)
{
	uint32_t v = 0;
	uint8_t n = idle_high ? 9u : 10u;
	if (!wire3_gpio_read(gpio_of(idle_high), CS_IF, 0x40u | addr, 7, pull_up, n, &v)) {
		return false;
	}
	if (!idle_high) {
		CHECK((v & 1u) == (pull_up ? 1u : 0u));	// after the 16th clock the chip is off the line
		v >>= 1;
	}
	*value = (uint8_t)v;
	*slot = (v & 0x100u) != 0u;
	return true;
}

static void test_if_read(bool idle_high)
{
	static const uint8_t patterns[] = { 0xA6u, 0x59u, 0x00u, 0xFFu, 0x80u, 0x01u };
	rig(idle_high);
	for (unsigned i = 0; i < sizeof(patterns); i++) {
		for (int pull = 0; pull < 2; pull++) {
			uint8_t v = 0;
			bool slot = false;
			ifc.reg[AK2365A_REG_CTRL4] = patterns[i];
			ifc.reg[AK2365A_REG_CTRL5] = (uint8_t)~patterns[i];
			CHECK(if_read(idle_high, AK2365A_REG_CTRL4, pull != 0, &v, &slot));
			CHECK(v == patterns[i]);
			CHECK(slot == (pull != 0));		// the slot before D7: nothing drives it, the pull shows
			CHECK(ifc.last_rises == 16);
			CHECK(!bus.host_drives);		// the line stays an input after a read
			CHECK(if_read(idle_high, AK2365A_REG_CTRL5, pull != 0, &v, &slot));
			CHECK(v == (uint8_t)~patterns[i]);
		}
	}
	CHECK(ifc.reads == (int)(4u * sizeof(patterns)));
	CHECK(ifc.writes == 0);					// a read frame writes nothing
	CHECK(bus.releases == ifc.reads);
	CHECK(bus.samples == ifc.reads * (idle_high ? 9 : 10));
	CHECK(synth.frames == 0);
	CHECK(lines_clean(idle_high));

	// A write after a read takes the line back, and the next read sees what was written
	const wire3_bus_t w3 = { wire3_gpio_write, (void *)gpio_of(idle_high) };
	CHECK(w3.write(w3.ctx, CS_IF, ak2365a_word(AK2365A_REG_CTRL4, 0x3Cu), AK2365A_WORD_BITS));
	uint8_t v = 0;
	bool slot = false;
	CHECK(if_read(idle_high, AK2365A_REG_CTRL4, false, &v, &slot));
	CHECK(v == 0x3Cu);
	CHECK(lines_clean(idle_high));
}

// A read with nothing answering shows the pull, with any head length, none included
static void test_read_counts(bool idle_high)
{
	rig(idle_high);
	uint32_t v = 0x12345678u;
	CHECK(wire3_gpio_read(gpio_of(idle_high), CS_SYNTH, 0, 0, true, 32, &v));
	CHECK(v == 0xFFFFFFFFu);
	// No rising edge after the last sample, but for the idle level
	CHECK(synth.last_clocks == (idle_high ? 32 : 31));
	CHECK(wire3_gpio_read(gpio_of(idle_high), CS_SYNTH, 0, 0, false, 1, &v));
	CHECK(v == 0u);
	CHECK(synth.last_clocks == (idle_high ? 1 : 0));
	CHECK(wire3_gpio_read(gpio_of(idle_high), CS_SYNTH, 0x80000001u, 32, false, 3, &v));
	CHECK(v == 0u);
	CHECK(synth.last_clocks == 32 + (idle_high ? 3 : 2));
	CHECK(bus.samples == 36 && bus.releases == 3);
	CHECK(lines_clean(idle_high));
}

// What cannot be sent is refused before any line moves
static void test_refusals(void)
{
	rig(false);
	uint32_t v = 0x55AA55AAu;
	wire3_gpio_t g = gpio_low;
	CHECK(!wire3_gpio_write((void *)&g, CS_SYNTH, 1, 0));
	CHECK(!wire3_gpio_write((void *)&g, CS_SYNTH, 1, 33));
	CHECK(!wire3_gpio_write(NULL, CS_SYNTH, 1, 16));
	CHECK(!wire3_gpio_read(NULL, CS_IF, 0x4Bu, 7, true, 10, &v));
	CHECK(!wire3_gpio_read(&g, CS_IF, 0x4Bu, 33, true, 10, &v));
	CHECK(!wire3_gpio_read(&g, CS_IF, 0x4Bu, 7, true, 0, &v));
	CHECK(!wire3_gpio_read(&g, CS_IF, 0x4Bu, 7, true, 33, &v));
	CHECK(!wire3_gpio_read(&g, CS_IF, 0x4Bu, 7, true, 10, NULL));
	CHECK(bus.calls == 0);
	// A write-only bus: no way to release or read the line
	g.data_release = NULL;
	CHECK(!wire3_gpio_read(&g, CS_IF, 0x4Bu, 7, true, 10, &v));
	CHECK(wire3_gpio_write((void *)&g, CS_SYNTH, 0x5005u, 16));
	CHECK(synth.n == 1 && synth.words[0] == 0x5005u);
	int calls = bus.calls;
	g = gpio_low;
	g.data_in = NULL;
	CHECK(!wire3_gpio_read(&g, CS_IF, 0x4Bu, 7, true, 10, &v));
	// A line missing: nothing can be timed or sent
	g = gpio_low;
	g.cs = NULL;
	CHECK(!wire3_gpio_write((void *)&g, CS_SYNTH, 1, 16));
	g = gpio_low;
	g.clk = NULL;
	CHECK(!wire3_gpio_write((void *)&g, CS_SYNTH, 1, 16));
	g = gpio_low;
	g.data = NULL;
	CHECK(!wire3_gpio_write((void *)&g, CS_SYNTH, 1, 16));
	g = gpio_low;
	g.delay = NULL;
	CHECK(!wire3_gpio_write((void *)&g, CS_SYNTH, 1, 16));
	CHECK(!wire3_gpio_read(&g, CS_IF, 0x4Bu, 7, true, 10, &v));
	CHECK(bus.calls == calls);
	CHECK(v == 0x55AA55AAu);
	CHECK(synth.frames == 1 && ifc.frames == 0);
}

// The clock found away from its idle level: it is put there before the select falls, so the chip never counts that
// edge as a bit
static void test_clock_parked(bool idle_high)
{
	rig(idle_high);
	bus.clk = !idle_high;
	CHECK(wire3_gpio_write((void *)gpio_of(idle_high), CS_SYNTH, 0x6123u, 16));
	CHECK(bus.edges_deselected == 1);
	CHECK(synth.last_clocks == 16 && synth.n == 1 && synth.words[0] == 0x6123u);
	CHECK(bus.timing == 0 && bus.clk == idle_high);
}

int main(void)
{
	for (int idle_high = 0; idle_high < 2; idle_high++) {
		test_synth(idle_high != 0);
		test_lengths(idle_high != 0);
		test_if_write(idle_high != 0);
		test_if_read(idle_high != 0);
		test_read_counts(idle_high != 0);
		test_clock_parked(idle_high != 0);
	}
	test_refusals();
	printf("wire3GpioTest: %s (%d failures)\n", failures ? "FAILED" : "passed", failures);
	return failures ? 1 : 0;
}
