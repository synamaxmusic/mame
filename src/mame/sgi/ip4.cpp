// license:BSD-3-Clause
// copyright-holders:Patrick Mackinlay

/*
 * Silicon Graphics Professional IRIS 4D/50 and 4D/70.
 *
 *   Year  Model  Board  CPU    Clock    I/D Cache    Code Name
 *   1987  4D/50  IP4    R2000  8MHz     64KiB/32KiB  Twin Tower
 *   1987  4D/70  IP4    R2000  12.5MHz  64KiB/32KiB  Twin Tower
 *
 * Sources:
 *   - VME-Eclipse CPU (VIP10) Specification, Silicon Graphics, Inc.
 *
 * TODO:
 *  - configurable ram size
 *  - diagnostics
 *  - VME bus
 *  - graphics
 *
 * WIP:
 *  - can boot to monitor
 *  - failing diagnostics: VME, duarts, lio interrupts, fpu
 */

/*
 * SCN2681AC1N40 x 2
 * SCN2681AC1N24
 * P8254
 * CXK5816PN-15L        2,048x8 SRAM
 * WD33C93-PL
 * DS1216?              SmartWatch RAM
 * SAA1099
 *
 * 16MHz
 * 10MHz
 * 8MHz
 * 3.6864MHz
 */

#include "emu.h"

// cpu and memory
#include "cpu/mips/mips1.h"

// other devices
#include "machine/ds1315.h"
#include "machine/mc68681.h"
#include "machine/nvram.h"
#include "machine/pit8253.h"
#include "machine/wd33c9x.h"
#include "sound/saa1099.h"

// buses and connectors
#include "machine/nscsi_bus.h"
#include "bus/nscsi/hd.h"
#include "bus/nscsi/cd.h"
#include "bus/rs232/rs232.h"
#include "bus/rs232/hlemouse.h"

#include "kbd.h"
#include "speaker.h"

#define LOG_PARITY (1U << 1)

//#define VERBOSE (LOG_PARITY)

#include "logmacro.h"

namespace {

class ip4_state : public driver_device
{
public:
	ip4_state(machine_config const &mconfig, device_type type, char const *tag)
		: driver_device(mconfig, type, tag)
		, m_cpu(*this, "cpu")
		, m_rtc(*this, "rtc")
		, m_pit(*this, "pit")
		, m_scsi(*this, "scsi:0:wd33c93")
		, m_duart(*this, "duart%u", 0U)
		, m_serial(*this, "serial%u", 0U)
		, m_saa(*this, "saa")
		, m_nvram(*this, "nvram", 0x800, ENDIANNESS_BIG)
		, m_leds(*this, "led%u", 0U)
	{
	}

	void pi4d50(machine_config &config);

protected:
	virtual void machine_start() override;
	virtual void machine_reset() override;

	void common(machine_config &config);
	void map(address_map &map);

	template <unsigned N> void lio_interrupt(int state) { lio_interrupt(N, state); }
	void lio_interrupt(unsigned number, int state);
	void scsi_drq(int state);

	u16 cpucfg_r() { return m_cpucfg; }
	void cpucfg_w(u16 data);

	void parity_r(offs_t offset, u32 &data, u32 mem_mask);
	void parity_w(offs_t offset, u32 &data, u32 mem_mask);

	enum cpucfg_mask : u16
	{
		CPUCFG_LEDS = 0x001f,
		CPUCFG_S01  = 0x0040, // enable serial ports 0,1
		CPUCFG_S23  = 0x0080, // enable serial ports 2,3
		CPUCFG_MAIL = 0x0100, // enable mailbox interrupts
		CPUCFG_SIN  = 0x0200, // VME sysreset (reset)
		CPUCFG_RPAR = 0x0400, // enable parity checking
		CPUCFG_SLA  = 0x0800, // enable slave accesses
		CPUCFG_ARB  = 0x1000, // enable VME arbiter
		CPUCFG_BAD  = 0x2000, // write bad parity
		CPUCFG_DOG  = 0x4000, // enable watchdog timout
		CPUCFG_AUX2 = 0x8000, // unused
	};

	enum parerr_mask : u8
	{
		PAR_LAN = 0x01,
		PAR_DMA = 0x02,
		PAR_CPU = 0x04,
		PAR_VME = 0x08,
		PAR_B3  = 0x10, // parity errory byte 3
		PAR_B2  = 0x20, // parity errory byte 2
		PAR_B1  = 0x40, // parity errory byte 1
		PAR_B0  = 0x80, // parity errory byte 0
		PAR_ALL = 0xf0, // parity errory all bytes
	};

	enum lio_int_number : unsigned
	{
		LIO_D0   = 0, // duart 0
		LIO_D1   = 1, // duart 1
		LIO_D2   = 2, // duart 2
					  // unused
		LIO_SCSI = 4, // scsi
					  // unused
		LIO_MAIL = 6, // VME mailbox
		LIO_AC   = 7, // VME AC fail
	};

private:
	required_device<mips1_device_base> m_cpu;

	required_device<ds1315_device> m_rtc;
	required_device<pit8254_device> m_pit;
	required_device<wd33c9x_base_device> m_scsi;
	required_device_array<scn2681_device, 3> m_duart;
	required_device_array<rs232_port_device, 4> m_serial;
	required_device<saa1099_device> m_saa;

	memory_share_creator<u8> m_nvram;

	output_finder<5> m_leds;

	// machine registers
	u16 m_cpucfg;
	u16 m_dmalo;
	u16 m_dmahi;
	u8 m_lio_isr;
	u8 m_parerr;
	u32 m_erradr;

	// other machine state
	std::unique_ptr<u8[]> m_parity;
	memory_passthrough_handler m_parity_mph;
	u32 m_parity_bad;
	bool m_lio_int;
};

void ip4_state::map(address_map &map)
{
	//map(0x1c00'0000, 0x1cff'ffff); // vme a24 modifier 0x3d privileged
	//map(0x1d00'0000, 0x1d00'ffff); // vme a16 modifier 0x2d privileged
	//map(0x1d10'0000, 0x1d10'ffff); // vme a16 modifier 0x29 non-privileged
	//map(0x1df0'0000, 0x1dff'ffff).umask32(0x0000'ff00); // VME_IACK: vme interrupt acknowledge
	//map(0x1e00'0000, 0x1eff'ffff); // vme a24 modifier 0x39 non-privileged

	// TODO: 4 banks of 4 SIMMs with parity
	map(0x0000'0000, 0x007f'ffff).ram();

	map(0x1f60'0000, 0x1f60'0003).umask32(0xff00'0000).w(m_saa, FUNC(saa1099_device::data_w));
	map(0x1f60'0010, 0x1f60'0013).umask32(0xff00'0000).w(m_saa, FUNC(saa1099_device::control_w));

	map(0x1f80'0000, 0x1f80'0003).umask32(0x00ff'0000).lr8(NAME([]() { return 0; })); // system id prom/coprocessor present

	//map(0x1f840000, 0x1f840003).umask32(0x0000'00ff).lrw8(NAME([this]() { return m_vme_isr; }), NAME([this](u8 data) { m_vme_isr = data; }));
	//map(0x1f840008, 0x1f84000b).umask32(0x0000'00ff).lrw8(NAME([this]() { return m_vme_imr; }), NAME([this](u8 data) { m_vme_imr = data; }));

	map(0x1f88'0000, 0x1f88'0003).umask32(0x0000'ffff).rw(FUNC(ip4_state::cpucfg_r), FUNC(ip4_state::cpucfg_w));

	map(0x1f90'0000, 0x1f90'0003).umask32(0x0000'ffff).lw16(NAME([this](u16 data) { m_dmalo = data; }));
	map(0x1f92'0000, 0x1f92'0003).umask32(0x0000'ffff).lw16(NAME([this](u16 data) { m_dmahi = data; }));
	map(0x1f94'0000, 0x1f94'0003).nopw(); // dma flush

	map(0x1f98'0000, 0x1f98'0003).umask32(0x0000'00ff).lr8(NAME([this]() { return m_lio_isr; }));

	map(0x1f9a'0000, 0x1f9a'0003).nopr(); // switches

	map(0x1fa0'0000, 0x1fa0'0003).umask32(0xff00'0000).lr8(NAME([this]() { m_cpu->set_input_line(INPUT_LINE_IRQ4, 0); return 0; }));
	map(0x1fa2'0000, 0x1fa2'0003).umask32(0xff00'0000).lr8(NAME([this]() { m_cpu->set_input_line(INPUT_LINE_IRQ2, 0); return 0; }));
	map(0x1fa4'0000, 0x1fa4'0003).lr32([this]() { return m_erradr; }, "sbe");
	map(0x1fa8'0000, 0x1fa8'0003).umask32(0xff00'0000).lr8(NAME([this]() { m_scsi->reset_w(0); return 0; }));
	map(0x1fa8'0004, 0x1fa8'0007).umask32(0xff00'0000).lr8(NAME([this]() { m_scsi->reset_w(1); return 0; }));

	//map(0x1fa60000, 0x1fa60003).umask32(0xff00'0000); // vme rmw
	map(0x1faa0000, 0x1faa0003).lrw8(
		NAME([this](offs_t offset) { m_parerr &= ~(PAR_ALL | (1U << offset)); return 0; }),
		NAME([this](offs_t offset, u8 data) { m_parerr &= ~(PAR_ALL | (1U << offset)); }));
	map(0x1faa0004, 0x1faa0007).umask32(0x00ff'0000).lr8(NAME([this]() { return m_parerr ^ PAR_ALL; }));

	map(0x1fae'0000, 0x1fae'001f).rom().region("idprom", 0);

	map(0x1fb0'0000, 0x1fb0'0003).umask32(0x00ff'0000).rw(m_scsi, FUNC(wd33c93_device::indir_addr_r), FUNC(wd33c93_device::indir_addr_w));
	map(0x1fb0'0100, 0x1fb0'0103).umask32(0x00ff'0000).rw(m_scsi, FUNC(wd33c93_device::indir_reg_r), FUNC(wd33c93_device::indir_reg_w));
	map(0x1fb4'0000, 0x1fb4'000f).umask32(0xff00'0000).rw(m_pit, FUNC(pit8254_device::read), FUNC(pit8254_device::write));

	map(0x1fb8'0000, 0x1fb8'00ff).umask32(0xff00'0000).lrw8(
		NAME([this](offs_t offset) { return m_duart[BIT(offset, 0, 2)]->read(offset >> 2); }),
		NAME([this](offs_t offset, u8 data) { m_duart[BIT(offset, 0, 2)]->write(offset >> 2, data); }));

	map(0x1fbc'0000, 0x1fbc'1fff).umask32(0xff00'0000).lrw8(
		NAME([this](offs_t offset) { return m_nvram[offset]; }),
		NAME([this](offs_t offset, u8 data) { m_nvram[offset] = data; }));

	map(0x1fc0'0000, 0x1fc3'ffff).rom().region("boot", 0);
}

static DEVICE_INPUT_DEFAULTS_START(ip4_ctl1)
	DEVICE_INPUT_DEFAULTS("VALID", 0x000f, 0x000f)
DEVICE_INPUT_DEFAULTS_END

static void scsi_devices(device_slot_interface &device)
{
	device.option_add("cdrom", NSCSI_CDROM_SGI).machine_config(
		[](device_t *device)
		{
			downcast<nscsi_cdrom_device &>(*device).set_block_size(512);
		});
	device.option_add("harddisk", NSCSI_HARDDISK);
}

void ip4_state::pi4d50(machine_config &config)
{
	R2000(config, m_cpu, 16_MHz_XTAL / 2, 65536, 32768);
	m_cpu->set_fpu(mips1_device_base::MIPS_R2010);

	common(config);
}

void ip4_state::common(machine_config &config)
{
	m_cpu->set_addrmap(AS_PROGRAM, &ip4_state::map);
	m_cpu->in_brcond<0>().set([]() { return 1; }); // writeback complete

	DS1315(config, m_rtc, 0); // DS1216?

	NVRAM(config, "nvram", nvram_device::DEFAULT_ALL_0); // CXK5816PN-15L

	PIT8254(config, m_pit);
	m_pit->set_clk<2>(3.6864_MHz_XTAL);
	m_pit->out_handler<0>().set([this](int state) { if (state) m_cpu->set_input_line(INPUT_LINE_IRQ2, 1); });
	m_pit->out_handler<1>().set([this](int state) { if (state) m_cpu->set_input_line(INPUT_LINE_IRQ4, 1); });
	m_pit->out_handler<2>().set(m_pit, FUNC(pit8254_device::write_clk0));
	m_pit->out_handler<2>().append(m_pit, FUNC(pit8254_device::write_clk1));

	NSCSI_BUS(config, "scsi");
	NSCSI_CONNECTOR(config, "scsi:0").option_set("wd33c93", WD33C93).machine_config(
		[this](device_t *device)
		{
			wd33c9x_base_device &wd33c93(downcast<wd33c9x_base_device &>(*device));

			wd33c93.set_clock(10'000'000);
			wd33c93.irq_cb().set(*this, FUNC(ip4_state::lio_interrupt<LIO_SCSI>)).invert();
			wd33c93.drq_cb().set(*this, FUNC(ip4_state::scsi_drq));
		});
	NSCSI_CONNECTOR(config, "scsi:1", scsi_devices, "harddisk", false);
	NSCSI_CONNECTOR(config, "scsi:2", scsi_devices, nullptr, false);
	NSCSI_CONNECTOR(config, "scsi:3", scsi_devices, nullptr, false);
	NSCSI_CONNECTOR(config, "scsi:4", scsi_devices, nullptr, false);
	NSCSI_CONNECTOR(config, "scsi:5", scsi_devices, nullptr, false);
	NSCSI_CONNECTOR(config, "scsi:6", scsi_devices, nullptr, false);
	NSCSI_CONNECTOR(config, "scsi:7", scsi_devices, nullptr, false);

	// duart 0 (keyboard/mouse)
	SCN2681(config, m_duart[0], 3.6864_MHz_XTAL); // SCN2681AC1N24
	sgi_kbd_port_device &keyboard_port(SGI_KBD_PORT(config, "keyboard_port", default_sgi_kbd_devices, nullptr));
	rs232_port_device &mouse_port(RS232_PORT(config, "mouse_port",
		[](device_slot_interface &device)
		{
			device.option_add("mouse", SGI_HLE_SERIAL_MOUSE);
		},
		nullptr));

	// duart 0 outputs
	m_duart[0]->irq_cb().set(FUNC(ip4_state::lio_interrupt<LIO_D0>)).invert();
	m_duart[0]->a_tx_cb().set(keyboard_port, FUNC(sgi_kbd_port_device::write_txd));
	m_duart[0]->b_tx_cb().set(mouse_port, FUNC(rs232_port_device::write_txd));

	// duart 0 inputs
	keyboard_port.rxd_handler().set(m_duart[0], FUNC(scn2681_device::rx_a_w));
	mouse_port.rxd_handler().set(m_duart[0], FUNC(scn2681_device::rx_b_w));

	// duart 1 (serial ports 0,1)
	SCN2681(config, m_duart[1], 3.6864_MHz_XTAL); // SCN2681AC1N40
	RS232_PORT(config, m_serial[0], default_rs232_devices, "terminal");
	RS232_PORT(config, m_serial[1], default_rs232_devices, nullptr);

	// duart 1 outputs
	m_duart[1]->irq_cb().set(FUNC(ip4_state::lio_interrupt<LIO_D1>)).invert();
	m_duart[1]->a_tx_cb().set(m_serial[0], FUNC(rs232_port_device::write_txd));
	m_duart[1]->b_tx_cb().set(m_serial[1], FUNC(rs232_port_device::write_txd));
	m_duart[1]->outport_cb().set(
		[this](u8 data)
		{
			m_serial[0]->write_rts(BIT(data, 0));
			m_serial[1]->write_rts(BIT(data, 1));
			m_duart[1]->ip5_w(BIT(data, 3));
			m_duart[1]->ip6_w(BIT(data, 3));
			m_serial[0]->write_dtr(BIT(data, 4));
			m_serial[1]->write_dtr(BIT(data, 5));
		});

	// duart 1 inputs
	m_serial[0]->rxd_handler().set(m_duart[1], FUNC(scn2681_device::rx_a_w));
	m_serial[0]->cts_handler().set(m_duart[1], FUNC(scn2681_device::ip0_w));
	m_serial[0]->dcd_handler().set(m_duart[1], FUNC(scn2681_device::ip3_w));

	m_serial[1]->rxd_handler().set(m_duart[1], FUNC(scn2681_device::rx_b_w));
	m_serial[1]->cts_handler().set(m_duart[1], FUNC(scn2681_device::ip1_w));
	m_serial[1]->dcd_handler().set(m_duart[1], FUNC(scn2681_device::ip2_w));

	// duart 2 (serial ports 2,3)
	SCN2681(config, m_duart[2], 3.6864_MHz_XTAL); // SCN2681AC1N40
	RS232_PORT(config, m_serial[2], default_rs232_devices, nullptr);
	RS232_PORT(config, m_serial[3], default_rs232_devices, nullptr);

	// duart 2 outputs
	m_duart[2]->irq_cb().set(FUNC(ip4_state::lio_interrupt<LIO_D2>)).invert();
	m_duart[2]->a_tx_cb().set(m_serial[2], FUNC(rs232_port_device::write_txd));
	m_duart[2]->b_tx_cb().set(m_serial[3], FUNC(rs232_port_device::write_txd));
	m_duart[2]->outport_cb().set(
		[this](u8 data)
		{
			m_serial[2]->write_rts(BIT(data, 0));
			m_serial[3]->write_rts(BIT(data, 1));
			m_duart[2]->ip5_w(BIT(data, 3));
			m_duart[2]->ip6_w(BIT(data, 3));
			m_serial[2]->write_dtr(BIT(data, 4));
			m_serial[3]->write_dtr(BIT(data, 5));
		});

	// duart 2 inputs
	m_serial[2]->rxd_handler().set(m_duart[2], FUNC(scn2681_device::rx_a_w));
	m_serial[2]->cts_handler().set(m_duart[2], FUNC(scn2681_device::ip0_w));
	m_serial[2]->dcd_handler().set(m_duart[2], FUNC(scn2681_device::ip3_w));

	m_serial[3]->rxd_handler().set(m_duart[2], FUNC(scn2681_device::rx_b_w));
	m_serial[3]->cts_handler().set(m_duart[2], FUNC(scn2681_device::ip1_w));
	m_serial[3]->dcd_handler().set(m_duart[2], FUNC(scn2681_device::ip2_w));

	SPEAKER(config, "lspeaker").front_left();
	SPEAKER(config, "rspeaker").front_right();

	SAA1099(config, m_saa, 8_MHz_XTAL);
	m_saa->add_route(0, "lspeaker", 0.5);
	m_saa->add_route(1, "rspeaker", 0.5);
}

void ip4_state::machine_start()
{
	m_leds.resolve();

	save_item(NAME(m_cpucfg));
	save_item(NAME(m_lio_isr));
	save_item(NAME(m_lio_int));
	save_item(NAME(m_dmalo));
	save_item(NAME(m_dmahi));
	save_item(NAME(m_erradr));
	save_item(NAME(m_parerr));

	m_cpucfg = 0;
	m_lio_isr = 0xff;
	m_lio_int = false;

	m_dmalo = 0;
	m_dmahi = 0;

	// install phantom rtc with a memory tap
	m_cpu->space(AS_PROGRAM).install_readwrite_tap(0x1fbc'1ffc, 0x1fbc'1fff, "rtc",
		[this](offs_t offset, u32 &data, u32 mem_mask)
		{
			if (m_rtc->chip_enable())
				data = u32(m_rtc->read_data()) << 24;
		},
		[this](offs_t offset, u32 &data, u32 mem_mask)
		{
			if (!m_rtc->chip_enable())
			{
				if (data)
					m_rtc->read_1();
				else
					m_rtc->read_0();
			}
			else
				m_rtc->write_data(data >> 24);
		});

	m_parity_bad = 0;
}

void ip4_state::machine_reset()
{
	m_erradr = 0;
	m_parerr = 0;
}

void ip4_state::lio_interrupt(unsigned number, int state)
{
	// record interrupt state
	if (state)
		m_lio_isr |= 1U << number;
	else
		m_lio_isr &= ~(1U << number);

	// update interrupt line
	bool const lio_int = !m_lio_isr;
	if (m_lio_int ^ lio_int)
	{
		m_lio_int = lio_int;
		m_cpu->set_input_line(INPUT_LINE_IRQ1, m_lio_int);
	}
}

void ip4_state::scsi_drq(int state)
{
	if (state)
	{
		u32 const addr = (u32(m_dmahi) << 12) | (m_dmalo & 0x0fff);

		if (m_dmalo & 0x8000)
			m_cpu->space(0).write_byte(addr, m_scsi->dma_r());
		else
			m_scsi->dma_w(m_cpu->space(0).read_byte(addr));

		m_dmalo = (m_dmalo + 1) & 0x8fff;

		if (!(m_dmalo & 0xfff))
			m_dmahi++;
	}
}

void ip4_state::cpucfg_w(u16 data)
{
	LOG("cpucfg_w 0x%04x\n", data);

	// update leds
	for (unsigned i = 0; i < 5; i++)
		m_leds[i] = BIT(data, i);

	if (BIT(data, 9))
		machine().schedule_soft_reset();

	if ((m_cpucfg ^ data) & CPUCFG_RPAR)
		LOGMASKED(LOG_PARITY, "parity checking %d\n", BIT(data, 10));

	if ((m_cpucfg ^ data) & CPUCFG_BAD)
	{
		LOGMASKED(LOG_PARITY, "write bad parity %d\n", BIT(data, 13));

		if ((data & CPUCFG_BAD) && !m_parity)
		{
			unsigned const ram_size = 8;

			LOGMASKED(LOG_PARITY, "bad parity activated %dM\n", ram_size);

			m_parity = std::make_unique<u8[]>(ram_size << (20 - 3));
			m_parity_mph = m_cpu->space(0).install_readwrite_tap(0, (ram_size << 20) - 1, "parity",
				std::bind(&ip4_state::parity_r, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3),
				std::bind(&ip4_state::parity_w, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));
		}
	}

	m_cpucfg = data;
}

void ip4_state::parity_r(offs_t offset, u32 &data, u32 mem_mask)
{
	if (m_cpucfg & CPUCFG_RPAR)
	{
		bool error = false;
		for (unsigned byte = 0; byte < 4; byte++)
		{
			if (BIT(mem_mask, 24 - byte * 8, 8) && BIT(m_parity[offset >> 3], BIT(offset, 2) * 4 + byte))
			{
				m_parerr |= (PAR_B0 >> byte) | PAR_CPU;
				error = true;

				LOGMASKED(LOG_PARITY, "bad parity err 0x%08x byte %d count %d\n", offset, byte, m_parity_bad);
			}
		}

		if (error)
		{
			m_erradr = offset;
			m_cpu->berr_w(1);
		}
	}
}

void ip4_state::parity_w(offs_t offset, u32 &data, u32 mem_mask)
{
	if (m_cpucfg & CPUCFG_BAD)
	{
		for (unsigned byte = 0; byte < 4; byte++)
		{
			if (BIT(mem_mask, 24 - byte * 8, 8) && !BIT(m_parity[offset >> 3], BIT(offset, 2) * 4 + byte))
			{
				m_parity[offset >> 3] |= 1U << (BIT(offset, 2) * 4 + byte);
				m_parity_bad++;

				LOGMASKED(LOG_PARITY, "bad parity set 0x%08x byte %d count %d\n", offset, byte, m_parity_bad);
			}
		}
	}
	else
	{
		for (unsigned byte = 0; byte < 4; byte++)
		{
			if (BIT(mem_mask, 24 - byte * 8, 8) && BIT(m_parity[offset >> 3], BIT(offset, 2) * 4 + byte))
			{
				m_parity[offset >> 3] &= ~(1U << (BIT(offset, 2) * 4 + byte));
				m_parity_bad--;

				LOGMASKED(LOG_PARITY, "bad parity clr 0x%08x byte %d count %d\n", offset, byte, m_parity_bad);
			}
		}

		if (m_parity_bad == 0)
		{
			LOGMASKED(LOG_PARITY, "bad parity deactivated\n");

			m_parity_mph.remove();
			m_parity.reset();
		}
	}
}

ROM_START(pi4d50)
	ROM_REGION32_BE(0x40000, "boot", 0)
	ROM_SYSTEM_BIOS(0, "4d1v3", "Version 4D1-3.0 PROM IP4 Mon Jan  4 20:29:51 PST 1988 SGI")
	ROMX_LOAD("070-0093-009.bin", 0x000000, 0x010000, CRC(261b0a4c) SHA1(59f73d0e022a502dc5528289e388700b51b308da), ROM_BIOS(0) | ROM_SKIP(3))
	ROMX_LOAD("070-0094-009.bin", 0x000001, 0x010000, CRC(8c05f591) SHA1(d4f86ad274f9dfe10c38551f3b6b9ba73570747f), ROM_BIOS(0) | ROM_SKIP(3))
	ROMX_LOAD("070-0095-009.bin", 0x000002, 0x010000, CRC(2dacfcb7) SHA1(0149274a11d61e3ada0f7b055e79d884a65481d3), ROM_BIOS(0) | ROM_SKIP(3))
	ROMX_LOAD("070-0096-009.bin", 0x000003, 0x010000, CRC(72dd0246) SHA1(6df99bdf7afaded8ef68a9644dd06ca69a996db0), ROM_BIOS(0) | ROM_SKIP(3))

	ROM_REGION32_BE(0x20, "idprom", 0)
	ROM_LOAD("idprom.bin", 0, 0x20, NO_DUMP)
ROM_END

} // anonymous namespace

//   YEAR  NAME    PARENT  COMPAT  MACHINE  INPUT  CLASS      INIT        COMPANY             FULLNAME                   FLAGS
COMP(1987, pi4d50, 0,      0,      pi4d50,  0,     ip4_state, empty_init, "Silicon Graphics", "Professional IRIS 4D/50", MACHINE_NOT_WORKING)
