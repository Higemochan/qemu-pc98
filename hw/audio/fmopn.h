/*
 * YAMAHA FM OPN Sound Generator
 * Copyright (C) Jarek Burczynski
 * Copyright (C) Tatsuyuki Satoh
 *
 * SPDX-License-Identifier: GPL-2.0+
 */

#ifndef HW_AUDIO_FMOPN_H
#define HW_AUDIO_FMOPN_H

typedef int32_t stream_sample_t;
typedef int32_t DEV_SMPL;
typedef void (*DEVCB_SRATE_CHG)(void *info, uint32_t newSRate);

/* --- select emulation chips --- */
#define BUILD_YM2203  0     // build YM2203(OPN)   emulator
#define BUILD_YM2608  1     // build YM2608(OPNA)  emulator
#define BUILD_YM2610  0     // build YM2610(OPNB)  emulator
#define BUILD_YM2610B 0     // build YM2610B(OPNB?)emulator
#define BUILD_YM2612  0     // build YM2612(OPN2)  emulator
#define BUILD_YM3438  0     // build YM3438(OPN2C) emulator

/* select timer system internal or external */
#define FM_INTERNAL_TIMER 1

/* --- speedup optimize --- */
/* busy flag emulation , The definition of FM_GET_TIME_NOW() is necessary. */
#define FM_BUSY_FLAG_SUPPORT 0

/* --- external SSG(YM2149/AY-3-8910)emulator interface port */
/* used by YM2203,YM2608,and YM2610 */
typedef struct _ssg_callbacks ssg_callbacks;
struct _ssg_callbacks
{
	void (*set_clock)(void *param, uint32_t clock);
	void (*write)(void *param, uint8_t address, uint8_t data);
	uint8_t (*read)(void *param);
	void (*reset)(void *param);
};

/* --- external callback functions for realtime update --- */

#if FM_BUSY_FLAG_SUPPORT
#define TIME_TYPE                   attotime
#define UNDEFINED_TIME              attotime::zero
#define FM_GET_TIME_NOW(machine)    (machine)->time()
#define ADD_TIMES(t1, t2)           ((t1) + (t2))
#define COMPARE_TIMES(t1, t2)       (((t1) == (t2)) ? 0 : ((t1) < (t2)) ? -1 : 1)
#define MULTIPLY_TIME_BY_INT(t,i)   ((t) * (i))
#endif



typedef void (*FM_TIMERHANDLER)(void *param,uint8_t c,int32_t cnt,uint32_t clock);
typedef void (*FM_IRQHANDLER)(void *param,uint8_t irq);
/* FM_TIMERHANDLER : Stop or Start timer         */
/* int n          = chip number                  */
/* int c          = Channel 0=TimerA,1=TimerB    */
/* int count      = timer count (0=stop)         */
/* doube stepTime = step time of one count (sec.)*/

/* FM_IRQHHANDLER : IRQ level changing sense     */
/* int n       = chip number                     */
/* int irq     = IRQ level 0=OFF,1=ON            */

#if BUILD_YM2203
/* -------------------- YM2203(OPN) Interface -------------------- */

/*
** Initialize YM2203 emulator(s).
**
** 'num'           is the number of virtual YM2203's to allocate
** 'baseclock'
** 'rate'          is sampling rate
** 'TimerHandler'  timer callback handler when timer start and clear
** 'IRQHandler'    IRQ callback handler when changed IRQ level
** return      0 = success
*/
void * ym2203_init(void *param, uint32_t baseclock, uint32_t rate,
				FM_TIMERHANDLER TimerHandler,FM_IRQHANDLER IRQHandler);

/*
** link SSG callbacks into the YM2203
*/
void ym2203_link_ssg(void *chip, const ssg_callbacks *ssg, void *ssg_param);

/*
** set the callback function for YM2203 sample rate changes
*/
void ym2203_set_srchg_cb(void *chip, DEVCB_SRATE_CHG cbFunc, void* dataPtr);

/*
** shutdown the YM2203 emulators
*/
void ym2203_shutdown(void *chip);

/*
** reset all chip registers for YM2203 number 'num'
*/
void ym2203_reset_chip(void *chip);

/*
** update one of chip
*/
void ym2203_update_one(void *chip, uint32_t length, DEV_SMPL **buffer);

/*
** Write
** return : InterruptLevel
*/
void ym2203_write(void *chip, uint8_t a, uint8_t v);

/*
** Read
** return : InterruptLevel
*/
uint8_t ym2203_read(void *chip, uint8_t a);

/*
**  Timer OverFlow
*/
uint8_t ym2203_timer_over(void *chip, uint8_t c);

/*
**  Channel Muting
*/
void ym2203_set_mutemask(void *chip, uint32_t MuteMask);
#endif /* BUILD_YM2203 */

#if BUILD_YM2608
/* -------------------- YM2608(OPNA) Interface -------------------- */
void * ym2608_init(void *param, uint32_t baseclock, uint32_t rate,
				FM_TIMERHANDLER TimerHandler,FM_IRQHANDLER IRQHandler);
void ym2608_link_ssg(void *chip, const ssg_callbacks *ssg, void *ssg_param);
//void ym2608_set_srchg_cb(void *chip, DEVCB_SRATE_CHG cbFunc, void* dataPtr);
void ym2608_shutdown(void *chip);
void ym2608_reset_chip(void *chip);
void ym2608_update_one(void *chip, uint32_t length, stream_sample_t **buffer);

void ym2608_write(void *chip, uint8_t a, uint8_t v);
uint8_t ym2608_read(void *chip, uint8_t a);
uint8_t ym2608_timer_over(void *chip, uint8_t c );
void ym2608_alloc_pcmromb(void* chip, uint32_t memsize);
void ym2608_write_pcmromb(void* chip, uint32_t offset, uint32_t length, const uint8_t* data);

void ym2608_set_mutemask(void *chip, uint32_t MuteMask);
/* Supply the external 0x2000-byte rhythm ADPCM-A ROM before ym2608_init(). */
void ym2608_set_rhythm_rom(const uint8_t *rom);
#endif /* BUILD_YM2608 */

#if (BUILD_YM2610||BUILD_YM2610B)
/* -------------------- YM2610(OPNB) Interface -------------------- */
void * ym2610_init(void *param, uint32_t baseclock, uint32_t rate,
				FM_TIMERHANDLER TimerHandler,FM_IRQHANDLER IRQHandler);
void ym2610_link_ssg(void *chip, const ssg_callbacks *ssg, void *ssg_param);
void ym2610_shutdown(void *chip);
void ym2610_reset_chip(void *chip);
void ym2610_update_one(void *chip, uint32_t length, DEV_SMPL **buffer);

#if BUILD_YM2610B
void ym2610b_update_one(void *chip, uint32_t length, DEV_SMPL **buffer);
#endif /* BUILD_YM2610B */

void ym2610_write(void *chip, uint8_t a, uint8_t v);
uint8_t ym2610_read(void *chip, uint8_t a);
uint8_t ym2610_timer_over(void *chip, uint8_t c );
void ym2610_alloc_pcmroma(void* chip, uint32_t memsize);
void ym2610_write_pcmroma(void* chip, uint32_t offset, uint32_t length, const uint8_t* data);
void ym2610_alloc_pcmromb(void* chip, uint32_t memsize);
void ym2610_write_pcmromb(void* chip, uint32_t offset, uint32_t length, const uint8_t* data);

void ym2610_set_mutemask(void *chip, uint32_t MuteMask);
#endif /* (BUILD_YM2610||BUILD_YM2610B) */

#if (BUILD_YM2612||BUILD_YM3438)
void * ym2612_init(void *param, uint32_t baseclock, uint32_t rate,
				FM_TIMERHANDLER TimerHandler,FM_IRQHANDLER IRQHandler);
void ym2612_shutdown(void *chip);
void ym2612_reset_chip(void *chip);
void ym2612_update_one(void *chip, uint32_t length, DEV_SMPL **buffer);

void ym2612_write(void *chip, uint8_t a, uint8_t v);
uint8_t ym2612_read(void *chip, uint8_t a);
uint8_t ym2612_timer_over(void *chip, uint8_t c );

void ym2612_set_mutemask(void *chip, uint32_t MuteMask);
void ym2612_setoptions(void *chip, uint32_t Flags);
#endif /* (BUILD_YM2612||BUILD_YM3438) */

#endif	// __FMOPN_H__
