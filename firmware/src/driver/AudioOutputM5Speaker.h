#ifndef _AUDIO_OUTPUT_M5_SPEAKER_H
#define _AUDIO_OUTPUT_M5_SPEAKER_H
#include <AudioOutput.h>

/// set M5Speaker virtual channel (0-7)
//static constexpr uint8_t m5spk_virtual_channel = 0;

class AudioOutputM5Speaker : public AudioOutput
{
  public:
    AudioOutputM5Speaker(m5::Speaker_Class* m5sound, uint8_t virtual_sound_channel = 0)
    {
      _m5sound = m5sound;
      _virtual_ch = virtual_sound_channel;
    }
    virtual ~AudioOutputM5Speaker(void) {};
    virtual bool begin(void) override { _underruns = 0; _flushes = 0; return true; }
    virtual bool ConsumeSample(int16_t sample[2]) override
    {
      if (_tri_buffer_index < tri_buf_size)
      {
        _tri_buffer[_tri_index][_tri_buffer_index  ] = sample[0];
        _tri_buffer[_tri_index][_tri_buffer_index+1] = sample[0];
        _tri_buffer_index += 2;

        return true;
      }

      flush();
      return false;
    }
    virtual void flush(void) override
    {
      if (_tri_buffer_index)
      {
        // ★ OWNER 2026-07-25 埋点: 要推下一块了, 队列却已经空 = 喇叭刚才断过料,
        // 也就是产生了一段可听见的空档。这是"一个字一个字/很多音被截断"的直接度量。
        // (第一块除外 —— 那时本来就还没排任何东西进去, 不算断料。)
        if (_flushes && _m5sound->isPlaying(_virtual_ch) == 0) { ++_underruns; }
        ++_flushes;
        _m5sound->playRaw(_tri_buffer[_tri_index], _tri_buffer_index, hertz, true, 1, _virtual_ch);
        _tri_index = _tri_index < 2 ? _tri_index + 1 : 0;
        _tri_buffer_index = 0;
        ++_update_count;
      }
    }
    // ★ OWNER 2026-07-25: 「音被截断/凑不成完整句子」的真凶就在这个函数里。
    // 原来是 flush() 之后立刻 _m5sound->stop(), 而 M5.Speaker::stop() 会把该通道
    // **已经排进 DMA 队列、但还没播出去**的样本直接丢掉。
    // AudioGeneratorWAV 撞到流末尾时是在 loop() 内部就调 stop() 的, 所以这段丢弃
    // 发生得非常早 —— 在 playWAV() 那层再去排空喇叭已经晚了(实测那样量到的
    // tail 恒等于 2~3ms, 因为队列早被清空了)。
    // 丢多少 = EOF 那一刻队列有多深:
    //   长句受数据到达速度限流, 队列几乎空 -> 只丢 13ms, 听不出来;
    //   短句整句一次到齐、生成器一路飙到 EOF, 队列压满 -> 丢 60~120ms,
    //   正好是最后一个音节 -> 听感就是"丢音、凑不成一个完整的句子"。
    // 修法: 先把队列放完再 stop。3 秒上限纯防呆(正常几十毫秒)。
    virtual bool stop(void) override
    {
      flush();
      uint32_t t0 = millis();
      while (_m5sound->isPlaying(_virtual_ch) && (millis() - t0 < 3000))
      {
        delay(1);
      }
      _m5sound->stop(_virtual_ch);
      for (size_t i = 0; i < 3; ++i)
      {
        memset(_tri_buffer[i], 0, tri_buf_size * sizeof(int16_t));
      }
      ++_update_count;
      return true;
    }

    const int16_t* getBuffer(void) const { return _tri_buffer[(_tri_index + 2) % 3]; }
    const uint32_t getUpdateCount(void) const { return _update_count; }
    size_t getUnderruns(void) const { return _underruns; }
    size_t getFlushes(void) const { return _flushes; }
    // 一块缓冲代表多少毫秒音频(拿来解释 underrun 的严重程度)
    uint32_t getChunkMs(void) const { return hertz ? (tri_buf_size / 2) * 1000UL / hertz : 0; }

  protected:
    m5::Speaker_Class* _m5sound;
    uint8_t _virtual_ch;
    // ★ OWNER 2026-07-25: 640 -> 4096。
    // 640 个 int16 = 320 立体声帧, @24kHz 只有 **13.3ms**;而 M5 每通道只有 2 个 wav 槽
    // (Speaker_Class::wav_info_t wavinfo[2]),所以在途音频总共才 ~27ms —— 主循环任何一次
    // 卡顿超过 27ms 喇叭就断料,产生可听见的空档。实测 loopCount 与 elapsed 的比值正好是
    // 每圈 13.3ms,说明主循环是被这个背压死死卡住走的,一点余量都没有。
    // 而 Robot::speech() 开口那一刻正好 gif_scene_start(GIF_SCENE_TALKING),加载+解码
    // GIF 的突发全压在头一秒 —— 短句整句都在突发里(一个字一个字),长句只有开头受影响
    // (听感"流畅连贯")。这正是主人报的长短句差异。
    // 4096 int16 = 2048 帧 = 85ms/块,2 槽在途 = **170ms 余量**,抬高一个量级。
    // 代价:3 × 4096 × 2 = 24KB(原 3.8KB)。RAM 用量 30% -> 仍有大量余量。
    static constexpr size_t tri_buf_size = 4096;
    int16_t _tri_buffer[3][tri_buf_size];
    size_t _tri_buffer_index = 0;
    size_t _tri_index = 0;
    size_t _update_count = 0;
    size_t _underruns = 0;   // ★ OWNER: 喇叭断料次数(每次 = 一段可听见的空档)
    size_t _flushes = 0;     // ★ OWNER: 推出去的块数, 给 underrun 当分母
};

#define FFT_SIZE 256
class fft_t
{
  float _wr[FFT_SIZE + 1];
  float _wi[FFT_SIZE + 1];
  float _fr[FFT_SIZE + 1];
  float _fi[FFT_SIZE + 1];
  uint16_t _br[FFT_SIZE + 1];
  size_t _ie;

public:
  fft_t(void)
  {
#ifndef M_PI
#define M_PI 3.141592653
#endif
    _ie = logf( (float)FFT_SIZE ) / log(2.0) + 0.5;
    static constexpr float omega = 2.0f * M_PI / FFT_SIZE;
    static constexpr int s4 = FFT_SIZE / 4;
    static constexpr int s2 = FFT_SIZE / 2;
    for ( int i = 1 ; i < s4 ; ++i)
    {
    float f = cosf(omega * i);
      _wi[s4 + i] = f;
      _wi[s4 - i] = f;
      _wr[     i] = f;
      _wr[s2 - i] = -f;
    }
    _wi[s4] = _wr[0] = 1;

    size_t je = 1;
    _br[0] = 0;
    _br[1] = FFT_SIZE / 2;
    for ( size_t i = 0 ; i < _ie - 1 ; ++i )
    {
      _br[ je << 1 ] = _br[ je ] >> 1;
      je = je << 1;
      for ( size_t j = 1 ; j < je ; ++j )
      {
        _br[je + j] = _br[je] + _br[j];
      }
    }
  }

  void exec(const int16_t* in)
  {
    memset(_fi, 0, sizeof(_fi));
    for ( size_t j = 0 ; j < FFT_SIZE / 2 ; ++j )
    {
      float basej = 0.25 * (1.0-_wr[j]);
      size_t r = FFT_SIZE - j - 1;

      /// perform han window and stereo to mono convert.
      _fr[_br[j]] = basej * (in[j * 2] + in[j * 2 + 1]);
      _fr[_br[r]] = basej * (in[r * 2] + in[r * 2 + 1]);
    }

    size_t s = 1;
    size_t i = 0;
    do
    {
      size_t ke = s;
      s <<= 1;
      size_t je = FFT_SIZE / s;
      size_t j = 0;
      do
      {
        size_t k = 0;
        do
        {
          size_t l = s * j + k;
          size_t m = ke * (2 * j + 1) + k;
          size_t p = je * k;
          float Wxmr = _fr[m] * _wr[p] + _fi[m] * _wi[p];
          float Wxmi = _fi[m] * _wr[p] - _fr[m] * _wi[p];
          _fr[m] = _fr[l] - Wxmr;
          _fi[m] = _fi[l] - Wxmi;
          _fr[l] += Wxmr;
          _fi[l] += Wxmi;
        } while ( ++k < ke) ;
      } while ( ++j < je );
    } while ( ++i < _ie );
  }

  uint32_t get(size_t index)
  {
    return (index < FFT_SIZE / 2) ? (uint32_t)sqrtf(_fr[ index ] * _fr[ index ] + _fi[ index ] * _fi[ index ]) : 0u;
  }
};

#endif