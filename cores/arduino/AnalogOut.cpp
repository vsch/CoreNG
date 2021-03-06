/*
 Copyright (c) 2011 Arduino.  All right reserved.

 This library is free software; you can redistribute it and/or
 modify it under the terms of the GNU Lesser General Public
 License as published by the Free Software Foundation; either
 version 2.1 of the License, or (at your option) any later version.

 This library is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 See the GNU Lesser General Public License for more details.

 You should have received a copy of the GNU Lesser General Public
 License along with this library; if not, write to the Free Software
 Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include "Core.h"
#include "AnalogOut.h"

#include "pwm/pwm.h"
#include "tc/tc.h"
#include "dacc/dacc.h"

// Initialise this module
extern void AnalogOutInit()
{
	// Nothing to do yet
}

// Convert a float in 0..1 to unsigned integer in 0..N
static inline uint32_t ConvertRange(float f, uint32_t top)
pre(0.0 <= ulValue; ulValue <= 1.0)
post(result <= top)
{
	return lround(f * (float)top);
}

// AnalogWrite to a DAC pin
// Return true if successful, false if we need to fall back to digitalWrite
static bool AnalogWriteDac(const PinDescription& pinDesc, float ulValue)
pre(0.0 <= ulValue; ulValue <= 1.0)
pre((pinDesc.ulPinAttribute & PIN_ATTR_DAC) != 0)
{
	const AnalogChannelNumber channel = pinDesc.ulADCChannelNumber;
	const uint32_t chDACC = ((channel == DA0) ? 0 : 1);
	if (dacc_get_channel_status(DACC) == 0)
	{
		// Enable clock for DACC_INTERFACE
		pmc_enable_periph_clk(ID_DACC);
		// Reset DACC registers
		dacc_reset(DACC);
		// Half word transfer mode
		dacc_set_transfer_mode(DACC, 0);
#if SAM4E
		// The SAM4E data sheet says we must also set this bit when using a peripheral clock frequency >100MHz. Not applicable to the SAM4S and SAME70.
		DACC->DACC_MR |= DACC_MR_CLKDIV_DIV_4;
#endif

#if (SAM3S) || (SAM3XA)
		/* Power save:
		 * sleep mode - 0 (disabled)
		 * fast wakeup - 0 (disabled)
		 */
		dacc_set_power_save(DACC, 0, 0);
		/* Timing:
		 * refresh - 0x08 (1024*8 dacc clocks)
		 * max speed mode - 0 (disabled)
		 * startup time - 0x10 (1024 dacc clocks)
		 */
		dacc_set_timing(DACC, 0x08, 0, 0x10);
#endif
#if !SAME70
		// Set up analog current
		dacc_set_analog_control(DACC, DACC_ACR_IBCTLCH0(0x02) | DACC_ACR_IBCTLCH1(0x02) | DACC_ACR_IBCTLDACCORE(0x01));
#endif
	}

#if !SAME70
	// Disable TAG
	dacc_set_channel_selection(DACC, chDACC);
#endif
	// Select output channel chDACC
	if ((dacc_get_channel_status(DACC) & (1 << chDACC)) == 0)
	{
		dacc_enable_channel(DACC, chDACC);
	}

	// Write user value - need to convert it from 8 to 12 bit resolution
#if SAME70
	dacc_write_conversion_data(DACC, ConvertRange(ulValue, (1 << DACC_RESOLUTION) - 1), chDACC);
#else
	dacc_write_conversion_data(DACC, ConvertRange(ulValue, (1 << DACC_RESOLUTION) - 1));
	while ((dacc_get_interrupt_status(DACC) & DACC_ISR_EOC) == 0) {}
#endif
	return true;
}

#if SAM3XA || SAME70
const unsigned int numPwmChannels = 8;
#elif SAM4E || SAM4S
const unsigned int numPwmChannels = 4;
#endif

static bool PWMEnabled = false;
static uint16_t PWMChanFreq[numPwmChannels] = {0};
static uint16_t PWMChanPeriod[numPwmChannels];

//***Temporary for debugging
uint32_t maxPwmLoopCount = 0;

// AnalogWrite to a PWM pin
// Return true if successful, false if we need to fall back to digitalWrite
static bool AnalogWritePwm(const PinDescription& pinDesc, float ulValue, uint16_t freq)
pre(0.0 <= ulValue; ulValue <= 1.0)
pre((pinDesc.ulPinAttribute & PIN_ATTR_PWM) != 0)
{
	const uint32_t chan = pinDesc.ulPWMChannel;
	if (freq == 0)
	{
		PWMChanFreq[chan] = freq;
		return false;
	}

	// Which PWM interface do we need to work with?
#if SAME70
	Pwm *PWMInterface = (chan <= 3) ? PWM0 : PWM1;
#else
	Pwm *PWMInterface = PWM;
#endif

	if (PWMChanFreq[chan] != freq)
	{
		if (!PWMEnabled)
		{
#if SAME70
			// PWM startup code for both PWM interfaces
			pmc_enable_periph_clk(ID_PWM0);
			pmc_enable_periph_clk(ID_PWM1);
			pwm_clock_t clockConfig;
			clockConfig.ul_clka = PwmSlowClock;
			clockConfig.ul_clkb = PwmFastClock;
			clockConfig.ul_mck = VARIANT_MCK;
			pwm_init(PWM0, &clockConfig);
			PWM0->PWM_SCM = 0;										// ensure no sync channels
			pwm_init(PWM1, &clockConfig);
			PWM1->PWM_SCM = 0;										// ensure no sync channels
#else
			// PWM Startup code
			pmc_enable_periph_clk(ID_PWM);
			pwm_clock_t clockConfig;
			clockConfig.ul_clka = PwmSlowClock;
			clockConfig.ul_clkb = PwmFastClock;
			clockConfig.ul_mck = VARIANT_MCK;
			pwm_init(PWM, &clockConfig);
			PWM->PWM_SCM = 0;										// ensure no sync channels
#endif
			PWMEnabled = true;
		}

		const bool useFastClock = (freq >= PwmFastClock/65535);
		const uint32_t period = ((useFastClock) ? PwmFastClock : PwmSlowClock)/freq;
		const uint32_t duty = ConvertRange(ulValue, period);

		PWMChanFreq[chan] = freq;
		PWMChanPeriod[chan] = (uint16_t)period;

		// Set up the PWM channel
		// We need to work around a bug in the SAM PWM channels. Enabling a channel is supposed to clear the counter, but it doesn't.
		// A further complication is that on the SAM3X, the update-period register doesn't appear to work.
		// So we need to make sure the counter is less than the new period before we change the period.
		for (unsigned int j = 0; j < 5; ++j)							// twice through should be enough, but just in case...
		{
			pwm_channel_disable(PWMInterface, chan);
			if (j > maxPwmLoopCount)
			{
				maxPwmLoopCount = j;
			}
			uint32_t oldCurrentVal = PWMInterface->PWM_CH_NUM[chan].PWM_CCNT & 0xFFFF;
			if (oldCurrentVal < period || oldCurrentVal > 65536 - 10)	// if counter is already small enough or about to wrap round, OK
			{
				break;
			}
			oldCurrentVal += 2;											// note: +1 doesn't work here, has to be at least +2
			PWMInterface->PWM_CH_NUM[chan].PWM_CPRD = oldCurrentVal;				// change the period to be just greater than the counter
			PWMInterface->PWM_CH_NUM[chan].PWM_CMR = PWM_CMR_CPRE_CLKB;			// use the fast clock to avoid waiting too long
			pwm_channel_enable(PWMInterface, chan);
			for (unsigned int i = 0; i < 1000; ++i)
			{
				const uint32_t newCurrentVal = PWMInterface->PWM_CH_NUM[chan].PWM_CCNT & 0xFFFF;
				if (newCurrentVal < period || newCurrentVal > oldCurrentVal)
				{
					break;												// get out when we have wrapped round, or failed to
				}
			}
		}

		pwm_channel_t channelConfig;
		memset(&channelConfig, 0, sizeof(channelConfig));				// clear unused fields
		channelConfig.channel = chan;
		channelConfig.ul_prescaler = (useFastClock) ? PWM_CMR_CPRE_CLKB : PWM_CMR_CPRE_CLKA;
		channelConfig.ul_duty = duty;
		channelConfig.ul_period = period;

		pwm_channel_init(PWMInterface, &channelConfig);
		pwm_channel_enable(PWMInterface, chan);

		// Now setup the PWM output pin for PWM this channel - do this after configuring the PWM to avoid glitches
		pio_configure(pinDesc.pPort,
				pinDesc.ulPinType,
				pinDesc.ulPin,
				pinDesc.ulPinConfiguration);
	}
	else
	{
		// We have to pass a pwm_channel_t struct to pwm_channel_update duty, but the only fields it reads are 'chan' and 'ul_period'.
		pwm_channel_t channelConfig;
		channelConfig.channel = chan;
		channelConfig.ul_period = (uint32_t)PWMChanPeriod[chan];
		pwm_channel_update_duty(PWMInterface, &channelConfig, ConvertRange(ulValue, channelConfig.ul_period));
	}
	return true;
}

#if SAM4S
const unsigned int numTcChannels = 6;
#elif SAM3XA || SAM4E
const unsigned int numTcChannels = 9;
#elif SAME70
const unsigned int numTcChannels = 12;
#endif

// Map from timer channel to TC channel number
static const uint8_t channelToChNo[numTcChannels] =
{
	0, 1, 2,
	0, 1, 2,
#if SAME70
	0, 1, 2,
	0, 1, 2
#endif
#if SAM3XA || SAM4E
	0, 1, 2
#endif
};

// Map from timer channel to TC number
static Tc * const channelToTC[numTcChannels] =
{
	TC0, TC0, TC0,
	TC1, TC1, TC1,
#if SAME70
	TC2, TC2, TC2,
	TC3, TC3, TC3
#endif
#if SAM3XA || SAM4E
	TC2, TC2, TC2
#endif
};

// Map from timer channel to TIO number
static const uint8_t channelToId[numTcChannels] =
{
	ID_TC0, ID_TC1, ID_TC2,
	ID_TC3, ID_TC4, ID_TC5,
#if SAME70
	ID_TC6, ID_TC7, ID_TC8,
	ID_TC9, ID_TC10, ID_TC11
#endif
#if SAM3XA || SAM4E
	ID_TC6, ID_TC7, ID_TC8
#endif
};

// Current frequency of each TC channel
static uint16_t TCChanFreq[numTcChannels] = {0};

static inline void TC_SetCMR_ChannelA(Tc *tc, uint32_t chan, uint32_t v)
{
	tc->TC_CHANNEL[chan].TC_CMR = (tc->TC_CHANNEL[chan].TC_CMR & 0xFFF0FFFF) | v;
}

static inline void TC_SetCMR_ChannelB(Tc *tc, uint32_t chan, uint32_t v)
{
	tc->TC_CHANNEL[chan].TC_CMR = (tc->TC_CHANNEL[chan].TC_CMR & 0xF0FFFFFF) | v;
}

static inline void TC_WriteCCR(Tc *tc, uint32_t chan, uint32_t v)
{
	tc->TC_CHANNEL[chan].TC_CCR = v;
}

// AnalogWrite to a TC pin
// Return true if successful, false if we need to fall back to digitalWrite
// WARNING: this will screw up big time if you try to use both the A and B outputs of the same timer at different frequencies.
// The DuetNG board uses only A outputs, so this is OK.
static bool AnalogWriteTc(const PinDescription& pinDesc, float ulValue, uint16_t freq)
pre(0.0 <= ulValue; ulValue <= 1.0)
pre((pinDesc.ulPinAttribute & PIN_ATTR_TIMER) != 0)
{
	const uint32_t chan = (uint32_t)pinDesc.ulTCChannel >> 1;
	if (freq == 0)
	{
		TCChanFreq[chan] = freq;
		return false;
	}
	else
	{
		Tc * const chTC = channelToTC[chan];
		const uint32_t chNo = channelToChNo[chan];
		const bool doInit = (TCChanFreq[chan] != freq);

		if (doInit)
		{
			TCChanFreq[chan] = freq;

			// Enable the peripheral clock to this timer
			pmc_enable_periph_clk(channelToId[chan]);

#if SAME70
			// Set up the timer mode and top count
			tc_init(chTC, chNo,
							TC_CMR_TCCLKS_TIMER_CLOCK2 |			// clock is MCLK/8 to save a little power and avoid overflow later on
							TC_CMR_WAVE |         					// Waveform mode
							TC_CMR_WAVSEL_UP_RC | 					// Counter running up and reset when equals to RC
							TC_CMR_EEVT_XC0 |     					// Set external events from XC0 (this setup TIOB as output)
							TC_CMR_ACPA_CLEAR | TC_CMR_ACPC_CLEAR |
							TC_CMR_BCPB_CLEAR | TC_CMR_BCPC_CLEAR |
							TC_CMR_ASWTRG_SET | TC_CMR_BSWTRG_SET);	// Software trigger will let us set the output high
			const uint32_t top = (VARIANT_MCK/8)/(uint32_t)freq;	// with 120MHz clock this varies between 228 (@ 65.535kHz) and 15 million (@ 1Hz)
#else
			// Set up the timer mode and top count
			tc_init(chTC, chNo,
							TC_CMR_TCCLKS_TIMER_CLOCK3 |			// clock is MCLK/32 to avoid overflow later on
							TC_CMR_WAVE |         					// Waveform mode
							TC_CMR_WAVSEL_UP_RC | 					// Counter running up and reset when equals to RC
							TC_CMR_EEVT_XC0 |     					// Set external events from XC0 (this setup TIOB as output)
							TC_CMR_ACPA_CLEAR | TC_CMR_ACPC_CLEAR |
							TC_CMR_BCPB_CLEAR | TC_CMR_BCPC_CLEAR |
							TC_CMR_ASWTRG_SET | TC_CMR_BSWTRG_SET);	// Software trigger will let us set the output high
			const uint32_t top = (VARIANT_MCK/32)/(uint32_t)freq;	// with 300MHz clock this varies between 143 (@ 65.535kHz) and 9.3 million (@ 1Hz)
#endif
			// The datasheet doesn't say how the period relates to the RC value, but from measurement it seems that we do not need to subtract one from top
			tc_write_rc(chTC, chNo, top);

			// When using TC channels to do PWM control of heaters with active low outputs on the Duet WiFi, if we don't take precautions
			// then we get a glitch straight after initialising the channel, because the compare output starts in the low state.
			// To avoid that, set the output high here if a high PWM was requested.
			if (ulValue >= 0.5)
			{
				TC_WriteCCR(chTC, chan, TC_CCR_SWTRG);
			}
		}

		const uint32_t threshold = ConvertRange(ulValue, tc_read_rc(chTC, chNo));
		if (threshold == 0)
		{
			if (((uint32_t)pinDesc.ulTCChannel & 1) == 0)
			{
				tc_write_ra(chTC, chNo, 1);
				TC_SetCMR_ChannelA(chTC, chNo, TC_CMR_ACPA_CLEAR | TC_CMR_ACPC_CLEAR);
			}
			else
			{
				tc_write_rb(chTC, chNo, 1);
				TC_SetCMR_ChannelB(chTC, chNo, TC_CMR_BCPB_CLEAR | TC_CMR_BCPC_CLEAR);
			}

		}
		else
		{
			if (((uint32_t)pinDesc.ulTCChannel & 1) == 0)
			{
				tc_write_ra(chTC, chNo, threshold);
				TC_SetCMR_ChannelA(chTC, chNo, TC_CMR_ACPA_CLEAR | TC_CMR_ACPC_SET);
			}
			else
			{
				tc_write_rb(chTC, chNo, threshold);
				TC_SetCMR_ChannelB(chTC, chNo, TC_CMR_BCPB_CLEAR | TC_CMR_BCPC_SET);
			}
		}

		if (doInit)
		{
			ConfigurePin(pinDesc);
			tc_start(chTC, chNo);
		}
	}
	return true;
}

// Analog write to DAC, PWM, TC or plain output pin
// Setting the frequency of a TC or PWM pin to zero resets it so that the next call to AnalogOut with a non-zero frequency
// will re-initialise it. The pinMode function relies on this.
void AnalogOut(Pin pin, float ulValue, uint16_t freq)
{
	if (pin > MaxPinNumber || std::isnan(ulValue))
	{
		return;
	}

	ulValue = constrain<float>(ulValue, 0.0, 1.0);

	const PinDescription& pinDesc = g_APinDescription[pin];
	const uint32_t attr = pinDesc.ulPinAttribute;
	if ((attr & PIN_ATTR_DAC) != 0)
	{
		if (AnalogWriteDac(pinDesc, ulValue))
		{
			return;
		}
	}
	else if ((attr & PIN_ATTR_PWM) != 0)
	{
		if (AnalogWritePwm(pinDesc, ulValue, freq))
		{
			return;
		}
	}
	else if ((attr & PIN_ATTR_TIMER) != 0)
	{
		if (AnalogWriteTc(pinDesc, ulValue, freq))
		{
			return;
		}
	}

	// Fall back to digital write
	pinMode(pin, (ulValue < 0.5) ? OUTPUT_LOW : OUTPUT_HIGH);
}

// End
