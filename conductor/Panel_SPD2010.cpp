/*----------------------------------------------------------------------------/
  Lovyan GFX - Graphics library for embedded devices.

Original Source:
 https://github.com/lovyan03/LovyanGFX/

Licence:
 [FreeBSD](https://github.com/lovyan03/LovyanGFX/blob/master/license.txt)

SPD2010 QSPI panel support (e.g. Guition JC3636W518C, 360x360 round).

 QSPI bus plumbing adapted from Panel_SH8601Z (author lovyan03 / ciniml /
 tobozo). Init register sequence adapted from Panel_ST77961 and the
 JC3636W518 manufacturer initialization code (scr_st77916.h).
/----------------------------------------------------------------------------*/

#if defined (ESP_PLATFORM)

#include "Panel_SPD2010.hpp"
#include "spd2010_init_cmds.h"
#include <lgfx/v1/Bus.hpp>
#include <lgfx/v1/platforms/common.hpp>
#include <lgfx/v1/misc/pixelcopy.hpp>
#include <lgfx/v1/misc/colortype.hpp>
#include "driver/spi_master.h"

#if defined LGFX_USE_QSPI

namespace lgfx
{
 inline namespace v1
 {
//----------------------------------------------------------------------------

  // ST779xx family command aliases
  static constexpr uint8_t CMD_SLPOUT   = 0x11;
  static constexpr uint8_t CMD_DISPON   = 0x29;
  static constexpr uint8_t CMD_CASET    = 0x2A;
  static constexpr uint8_t CMD_RASET    = 0x2B;
  static constexpr uint8_t CMD_RAMWR    = 0x2C;
  static constexpr uint8_t CMD_MADCTL   = 0x36;
  static constexpr uint8_t CMD_COLMOD   = 0x3A;
  static constexpr uint8_t CMD_INVOFF   = 0x20;
  static constexpr uint8_t CMD_INVON    = 0x21;

  // MADCTL bits
  static constexpr uint8_t MAD_MY  = 0x80;
  static constexpr uint8_t MAD_MX  = 0x40;
  static constexpr uint8_t MAD_MV  = 0x20;
  static constexpr uint8_t MAD_RGB = 0x00;
  static constexpr uint8_t MAD_BGR = 0x08;

  // Per-rotation MADCTL base (mirror/exchange bits, no color-order bit).
  static constexpr uint8_t madctl_table[8] = {
    MAD_RGB,                    // 0
    MAD_MV | MAD_MX,            // 1
    MAD_MX | MAD_MY,            // 2
    MAD_MV | MAD_MY,            // 3
    MAD_MY,                     // 4 (mirror)
    MAD_MV,                     // 5
    MAD_MX,                     // 6
    MAD_MV | MAD_MX | MAD_MY,   // 7
  };

  const uint8_t* Panel_SPD2010::getInitCommands(uint8_t listno) const
  {
    // The whole sequence is one list; see spd2010_init_cmds.h, which is
    // generated from Waveshare's driver rather than typed by hand.
    switch (listno)
    {
    case 0: return spd2010_init_cmds;
    default: return nullptr;
    }
  }

  bool Panel_SPD2010::init(bool use_reset)
  {
    if (!Panel_Device::init(use_reset)) {
      return false;
    }

    // Run the long vendor pre-configuration sequence
    command_list(getInitCommands(0));

    // Send MADCTL (color order + rotation) via command_list so it lands
    // reliably on QSPI panels. Inversion and color depth are applied by the
    // framework immediately after init() returns (invertDisplay / setColorDepth).
    uint8_t madctl = madctl_table[_internal_rotation & 7];
    madctl |= _cfg.rgb_order ? MAD_RGB : MAD_BGR;

    const uint8_t cmds[] =
    {
        CMD_MADCTL, 1, madctl,
        0xFF, 0xFF
    };
    command_list(cmds);

    setColorDepth(rgb565_2Byte);  // sends CMD_COLMOD (0x3A)

    // Seed _invert from cfg so the framework's invertDisplay() call in
    // init_impl() reinforces the right state.
    _invert = _cfg.invert;

    return true;
  }

  void Panel_SPD2010::update_madctl(void)
  {
    uint8_t madctl = madctl_table[_internal_rotation & 7];
    madctl |= _cfg.rgb_order ? MAD_RGB : MAD_BGR;

    const uint8_t cmds[] = {
        CMD_MADCTL, 1, madctl,
        0xFF, 0xFF
    };
    command_list(cmds);
  }

  void Panel_SPD2010::setRotation(uint_fast8_t r)
  {
    r &= 7;
    _rotation = r;
    _internal_rotation = ((r + _cfg.offset_rotation) & 3) | ((r & 4) ^ (_cfg.offset_rotation & 4));

    auto ox = _cfg.offset_x;
    auto oy = _cfg.offset_y;
    auto pw = _cfg.panel_width;
    auto ph = _cfg.panel_height;
    auto mw = _cfg.memory_width;
    auto mh = _cfg.memory_height;
    if (_internal_rotation & 1)
    {
      std::swap(ox, oy);
      std::swap(pw, ph);
      std::swap(mw, mh);
    }
    _width  = pw;
    _height = ph;
    _colstart = (_internal_rotation & 2) ? mw - (pw + ox) : ox;
    _rowstart = ((1 << _internal_rotation) & 0b10010110) ? mh - (ph + oy) : oy;
    _xs = _xe = _ys = _ye = INT16_MAX;

    update_madctl();
  }

  void Panel_SPD2010::setInvert(bool invert)
  {
    _invert = invert;
    startWrite();
    cs_control(false);
    write_cmd(invert ? CMD_INVON : CMD_INVOFF);
    _bus->wait();
    cs_control(true);
    endWrite();
  }

  void Panel_SPD2010::setSleep(bool flg)
  {
    startWrite();
    cs_control(false);
    if (flg) {
      write_cmd(0x10); // sleep in
    } else {
      write_cmd(CMD_SLPOUT);
      delay(120);
    }
    _bus->wait();
    cs_control(true);
    endWrite();
  }

  void Panel_SPD2010::setPowerSave(bool) {}
  void Panel_SPD2010::waitDisplay(void) {}
  bool Panel_SPD2010::displayBusy(void) { return false; }

  color_depth_t Panel_SPD2010::setColorDepth(color_depth_t depth)
  {
    uint8_t cmd_send = 0;
    if (depth == rgb565_2Byte) { cmd_send = 0x55; }
    else if (depth == rgb666_3Byte) { cmd_send = 0x66; }
    else { return _write_depth; }
    _write_depth = depth;
    _read_depth = depth;
    _write_bits = ((int)depth & color_depth_t::bit_mask);
    _read_bits  = _write_bits;

    startWrite();
    cs_control(false);
    write_cmd(CMD_COLMOD);
    _bus->writeCommand(cmd_send, 8);
    _bus->wait();
    cs_control(true);
    endWrite();

    return _write_depth;
  }

  void Panel_SPD2010::command_list(const uint8_t *addr)
  {
    startWrite();
    for (;;)
    {
      uint8_t cmd = *addr++;
      uint8_t num = *addr++;
      if (cmd == 0xFF && num == 0xFF) break;

      // write_cmd() handles CS cycling (deassert prev, assert for this cmd).
      this->write_cmd(cmd);
      uint_fast8_t ms = num & CMD_INIT_DELAY;
      num &= ~CMD_INIT_DELAY;
      if (num)
      {
        // Parameters follow the command with CS still asserted, on the data phase.
        do {
          _bus->writeCommand(*addr++, 8);
        } while (--num);
      }
      _bus->wait();

      if (ms)
      {
        ms = *addr++;
        cs_control(true);
        delay( (ms == 255 ? 500 : ms) );
        cs_control(false);
      }
    }
    _bus->wait();
    cs_control(true);
    endWrite();
  }

  void Panel_SPD2010::write_cmd(uint8_t cmd)
  {
    // QSPI command frame: 0x02 opcode, then 8-bit cmd in the 3rd byte.
    // Bytes on wire: 0x02 0x00 <cmd> 0x00. Deassert/reassert CS around it so
    // the command phase is cleanly delimited from the following data phase.
    _bus->wait();
    cs_control(true);
    uint8_t cmd_buffer[4] = {0x02, 0x00, 0x00, 0x00};
    cmd_buffer[2] = cmd;
    cs_control(false);
    for (int i = 0; i < 4; i++) {
      _bus->writeCommand(cmd_buffer[i], 8);
    }
  }

  void Panel_SPD2010::start_qspi(void)
  {
    // Begin pixel data transmission: single 32-bit command frame for RAMWR.
    // Bytes on wire: 0x32 0x00 0x2C 0x00 (0x32 opcode = 4-wire color write).
    // Sending it as ONE 32-bit writeCommand (not four 8-bit) is required so the
    // SPI peripheral delimits the command phase before the quad data phase.
    _bus->wait();
    cs_control(true);
    cs_control(false);
    _bus->writeCommand(0x002C0032, 32);
  }

  void Panel_SPD2010::end_qspi(void)
  {
    // Intentionally empty: CS stays asserted after pixel data. The next
    // write_cmd()/start_qspi() deasserts CS, matching the working QSPI panels.
  }

  void Panel_SPD2010::beginTransaction(void)
  {
    if (_in_transaction) return;
    _in_transaction = true;
    _bus->beginTransaction();
  }

  void Panel_SPD2010::endTransaction(void)
  {
    if (!_in_transaction) return;
    _in_transaction = false;

    if (_has_align_data)
    {
      _has_align_data = false;
      _bus->writeData(0, 8);
    }
    _bus->endTransaction();
  }

  void Panel_SPD2010::write_bytes(const uint8_t* data, uint32_t len, bool use_dma)
  {
    start_qspi();
    _bus->writeBytes(data, len, true, use_dma);
  }

  void Panel_SPD2010::setWindow(uint_fast16_t xs, uint_fast16_t ys, uint_fast16_t xe, uint_fast16_t ye)
  {
    if ((xe - xs) >= _width)  { xs = 0; xe = _width  - 1; }
    if ((ye - ys) >= _height) { ys = 0; ye = _height - 1; }

    xs += _colstart; xe += _colstart;
    ys += _rowstart; ye += _rowstart;

    // CASET: send the 4 address bytes as ONE 32-bit data word (big-endian).
    write_cmd(CMD_CASET);
    _bus->writeCommand(__builtin_bswap32((uint32_t)xs << 16 | xe), 32);

    // RASET
    write_cmd(CMD_RASET);
    _bus->writeCommand(__builtin_bswap32((uint32_t)ys << 16 | ye), 32);
    _bus->wait();
  }

  void Panel_SPD2010::writeBlock(uint32_t rawcolor, uint32_t len)
  {
    start_qspi();
    _bus->writeDataRepeat(rawcolor, _write_bits, len);
  }

  void Panel_SPD2010::writePixels(pixelcopy_t* param, uint32_t len, bool use_dma)
  {
    start_qspi();
    if (param->no_convert) {
      _bus->writeBytes(reinterpret_cast<const uint8_t*>(param->src_data), len * _write_bits >> 3, true, use_dma);
    } else {
      _bus->writePixels(param, len);
    }
    if (_cfg.dlen_16bit && (_write_bits & 15) && (len & 1)) {
      _has_align_data = !_has_align_data;
    }
    _bus->wait();
  }

  void Panel_SPD2010::drawPixelPreclipped(uint_fast16_t x, uint_fast16_t y, uint32_t rawcolor)
  {
    setWindow(x, y, x, y);
    if (_cfg.dlen_16bit) { _has_align_data = (_write_bits & 15); }
    start_qspi();
    _bus->writeData(rawcolor, _write_bits);
  }

  void Panel_SPD2010::writeFillRectPreclipped(uint_fast16_t x, uint_fast16_t y, uint_fast16_t w, uint_fast16_t h, uint32_t rawcolor)
  {
    uint32_t len = w * h;
    uint_fast16_t xe = w + x - 1;
    uint_fast16_t ye = y + h - 1;

    setWindow(x, y, xe, ye);
    start_qspi();
    _bus->writeDataRepeat(rawcolor, _write_bits, len);
  }

  void Panel_SPD2010::writeImage(uint_fast16_t x, uint_fast16_t y, uint_fast16_t w, uint_fast16_t h, pixelcopy_t* param, bool use_dma)
  {
    auto bytes = param->dst_bits >> 3;
    auto src_x = param->src_x;

    if (param->transp == pixelcopy_t::NON_TRANSP)
    {
      if (param->no_convert)
      {
        auto wb = w * bytes;
        uint32_t i = (src_x + param->src_y * param->src_bitwidth) * bytes;
        auto src = &((const uint8_t*)param->src_data)[i];
        setWindow(x, y, x + w - 1, y + h - 1);
        if (param->src_bitwidth == w || h == 1)
        {
          write_bytes(src, wb * h, use_dma);
        }
        else
        {
          auto add = param->src_bitwidth * bytes;
          if (use_dma)
          {
            if (_cfg.dlen_16bit && ((wb * h) & 1))
            {
              _has_align_data = !_has_align_data;
            }
            do
            {
              _bus->addDMAQueue(src, wb);
              src += add;
            } while (--h);
            _bus->execDMAQueue();
          }
          else
          {
            do
            {
              write_bytes(src, wb, false);
              src += add;
            } while (--h);
          }
        }
      }
      else
      {
        if (!_bus->busy())
        {
          static constexpr uint32_t WRITEPIXELS_MAXLEN = 32767;

          setWindow(x, y, x + w - 1, y + h - 1);
          bool nogap = (h == 1) || (param->src_y32_add == 0 && ((param->src_bitwidth << pixelcopy_t::FP_SCALE) == (w * param->src_x32_add)));
          if (nogap && (w * h <= WRITEPIXELS_MAXLEN))
          {
            writePixels(param, w * h, use_dma);
          }
          else
          {
            uint_fast16_t h_step = nogap ? WRITEPIXELS_MAXLEN / w : 1;
            uint_fast16_t h_len = (h_step > 1) ? ((h - 1) % h_step) + 1 : 1;
            writePixels(param, w * h_len, use_dma);
            if (h -= h_len)
            {
              param->src_y += h_len;
              do
              {
                param->src_x = src_x;
                writePixels(param, w * h_step, use_dma);
                param->src_y += h_step;
              } while (h -= h_step);
            }
          }
        }
        else
        {
          size_t wb = w * bytes;
          auto buf = _bus->getDMABuffer(wb);
          param->fp_copy(buf, 0, w, param);
          setWindow(x, y, x + w - 1, y + h - 1);
          write_bytes(buf, wb, true);
          _has_align_data = (_cfg.dlen_16bit && (_write_bits & 15) && (w & h & 1));
          while (--h)
          {
            param->src_x = src_x;
            param->src_y++;
            buf = _bus->getDMABuffer(wb);
            param->fp_copy(buf, 0, w, param);
            write_bytes(buf, wb, true);
          }
        }
      }
    }
    else
    {
      h += y;
      uint32_t wb = w * bytes;
      do
      {
        uint32_t i = 0;
        while (w != (i = param->fp_skip(i, w, param)))
        {
          auto buf = _bus->getDMABuffer(wb);
          int32_t len = param->fp_copy(buf, 0, w - i, param);
          setWindow(x + i, y, x + i + len - 1, y);
          write_bytes(buf, len * bytes, true);
          if (w == (i += len)) break;
        }
        param->src_x = src_x;
        param->src_y++;
      } while (++y != h);
    }
  }

  uint32_t Panel_SPD2010::readCommand(uint_fast16_t, uint_fast8_t, uint_fast8_t) { return 0; }
  uint32_t Panel_SPD2010::readData(uint_fast8_t, uint_fast8_t) { return 0; }
  void Panel_SPD2010::readRect(uint_fast16_t, uint_fast16_t, uint_fast16_t, uint_fast16_t, void*, pixelcopy_t*) {}

//----------------------------------------------------------------------------
 }
}

#endif // LGFX_USE_QSPI
#endif // ESP_PLATFORM