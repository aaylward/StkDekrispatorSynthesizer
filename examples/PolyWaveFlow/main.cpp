// 
// This program performs simple voice management if all noteOn and
// noteOff events are on channel 0.  Otherwise, channel values > 0 are
// mapped to specific string numbers. By default, the program creates
// a 6-string guitar.  If the normalized noteOn() velocity is < 0.2, a
// string is undamped but not plucked (this is implemented in the
// stk::Guitar class).  Thus, you can lightly depress a key on a MIDI
// keyboard and then experiment with string coupling.
//
// The Tcl/Tk GUI allows you to experiment with various parameter
// settings and that can be used in conjunction with a MIDI keyboard
// as: wish < tcl/EGuitar.tcl | ./eguitar -or -ip -im 1
//
// For the moment, this program does not support pitch bends.
//
// Gary P. Scavone, McGill University 2012.

#include "VoiceWave.h"
#include "SKINImsg.h"
#include "WvOut.h"
#include "JCRev.h"
#include "Skini.h"
#include "RtAudio.h"
#include "Delay.h"
#include "Cubic.h"

// Miscellaneous command-line parsing and instrument allocation
// functions are defined in utilites.cpp ... specific to this program.
#include "../eguitar/utilities.h"

#include <signal.h>
#include <iostream>
#include <algorithm>
#include <cmath>
using std::min;

static bool done;
static void finish(int ignore){ done = true; }

using namespace stk;

const unsigned int nStrings = 4;//12;

// Data structure for string information.
struct StringInfo{
  bool         inUse;            // is this string being used?
  unsigned int iNote;            // note number associated with this string
   
  StringInfo() : inUse(false), iNote(0) {};
};

// The TickData structure holds all the class instances and data that
// are shared by the various processing functions.
struct TickData {
  WvOut **wvout;
  VoiceWave* guitar;
	
  StringInfo voices[nStrings];
  JCRev reverb;
  Messager messager1;
  Messager messager2;
	
  Skini::Message message;
  StkFloat volume;
  StkFloat t60;
  unsigned int nWvOuts;
  int channels;
  int counter;
  bool realtime;
  bool settling;
  bool haveMessage;
  int keysDown;

  StkFloat feedbackGain;
  StkFloat oldFeedbackGain;
  StkFloat distortionGain;
  StkFloat distortionMix;
  Delay feedbackDelay;
  Cubic distortion;
  StkFloat feedbackSample;

  // Default constructor.
  TickData()
    : wvout(0), volume(22.0), t60(0.75),
      nWvOuts(0), channels(2), counter(0),
      realtime( false ), settling( false ), haveMessage( false ),
      keysDown(0), feedbackSample( 0.0 ) {}
};

#define DELTA_CONTROL_TICKS 30 // default sample frames between control input checks

// The processMessage() function encapsulates the handling of control
// messages.  It can be easily relocated within a program structure
// depending on the desired scheduling scheme.
static void processMessage( TickData* data )
{
  register StkFloat value1 = data->message.floatValues[0];
  register StkFloat value2 = data->message.floatValues[1];
  unsigned int channel = (unsigned int) data->message.channel;
	
  switch( data->message.type ) {

  case __SK_Exit_:
    if ( data->settling == false ) goto settle;
    done = true;
    return;

  case __SK_NoteOn_:
    if ( value2 > 0.0 ) { // velocity > 0
      unsigned int iNote = data->message.intValues[0];
      if ( channel == 0 ) { // do basic voice management
        unsigned int s;
        if ( data->keysDown >= (int) nStrings ) break; // ignore extra note on's
        // Find first unused string
        for ( s=0; s<nStrings; s++ )
          if ( !data->voices[s].inUse ) break;
        if ( s == nStrings ) break;
        data->voices[s].inUse = true;
        data->voices[s].iNote = iNote;
		 
		  
        data->guitar->noteOn( Midi2Pitch[iNote], value2 * ONE_OVER_128, s );

        data->keysDown++;
        // If first key down, turn on feedback gain
        if ( data->keysDown == 1 )
          data->feedbackGain = data->oldFeedbackGain;
      }
      else if ( channel <= nStrings )
        data->guitar->noteOn( Midi2Pitch[iNote], value2 * ONE_OVER_128, channel-1 );
      break;
    }
    // else a note off, so continue to next case

  case __SK_NoteOff_:
    if ( channel == 0 ) { // do basic voice management
      if ( !data->keysDown ) break;
      // Search for the released note
      unsigned int s, iNote;
      iNote = data->message.intValues[0];
      for ( s=0; s<nStrings; s++ )
        if ( data->voices[s].inUse && iNote == data->voices[s].iNote )
          break;
      if ( s == nStrings ) break;
      data->voices[s].inUse = false;
      data->guitar->noteOff( value2 * ONE_OVER_128, s );
      data->keysDown--;
      if ( data->keysDown == 0 ) { // turn off feedback gain and clear delay
        data->feedbackDelay.clear();
        data->feedbackGain = 0.0;
      }
    }
    else if ( channel <= nStrings )
      data->guitar->noteOff( value2 * ONE_OVER_128, channel-1 );
    break;

  case __SK_ControlChange_:
    if ( value1 == 16.f)//44.0 )
      data->reverb.setEffectMix( value2  * ONE_OVER_128 );
    else if ( value1 == 23.0)//7.0 )
      data->volume = value2 * ONE_OVER_128;
    else if ( value1 == 17.0)//27 ) // feedback delay
      data->feedbackDelay.setDelay( (value2 * Stk::sampleRate() / 127) + 1 );
    else if ( value1 == 18.0){//28 ) { // feedback gain
      //data->oldFeedbackGain = value2 * 0.01 / 127.0;
      data->oldFeedbackGain = value2 * 0.02 / 127.0;
      data->feedbackGain = data->oldFeedbackGain;
    }
    else if ( value1 == 19)//71 ) // pre-distortion gain
      data->distortionGain = 2.0 * value2 * ONE_OVER_128;
    else if ( value1 == 20)//72 ) // distortion mix
      data->distortionMix = value2 * ONE_OVER_128;
    else
      data->guitar->controlChange( (int) value1, value2 );
    break;
  case __SK_AfterTouch_:
    data->guitar->controlChange( 128, value1 );
    break;

  case __SK_PitchBend_:
    //  Implement me!
    break;
  case __SK_Volume_:
    data->volume = value1 * ONE_OVER_128;
    break;

  } // end of switch

	data->haveMessage = false;
  return;

 settle:
  // Exit and program change messages are preceeded with a short settling period.
  for ( unsigned int s=0; s<nStrings; s++ )
    if ( data->voices[s].inUse ) data->guitar->noteOff( 0.6, s );
  data->counter = (int) (0.3 * data->t60 * Stk::sampleRate());
  data->settling = true;
}


// The tick() function handles sample computation and scheduling of
// control updates.  If doing realtime audio output, it will be called
// automatically when the system needs a new buffer of audio samples.
static int tick( void *outputBuffer, void *inputBuffer, unsigned int nBufferFrames,
          double streamTime, RtAudioStreamStatus status, void *dataPointer )
{
  TickData *data = (TickData *) dataPointer;
  register StkFloat temp, sample, *samples = (StkFloat *) outputBuffer;
  int counter, nTicks = (int) nBufferFrames;

  while ( nTicks > 0 && !done ) {

    if ( !data->haveMessage ) {
		
      data->messager1.popMessage( data->message );
      if ( data->message.type > 0 ) {
        data->counter = (long) (data->message.time * Stk::sampleRate());
        data->haveMessage = true;
      }
      else
	  {
		  data->messager2.popMessage( data->message );
		  if ( data->message.type > 0 ) {
			  data->counter = (long) (data->message.time * Stk::sampleRate());
			  data->haveMessage = true;
		  }
		else
			data->counter = DELTA_CONTROL_TICKS;
		  
	  }
    }

    counter = min( nTicks, data->counter );
    data->counter -= counter;
    for ( int i=0; i<counter; i++ ) {

      // Put the previous distorted sample thru feedback
      sample = data->feedbackDelay.tick( data->feedbackSample * data->feedbackGain );
      sample = data->guitar->tick( sample );

      // Apply distortion (x - x^3/3) and mix
      temp = data->distortionGain * sample;
      if ( temp > 0.6666667 ) temp = 0.6666667;
      else if ( temp < -0.6666667 ) temp = -0.6666667;
      else temp = data->distortion.tick( temp );
      sample = (data->distortionMix * temp) + ((1 - data->distortionMix) * sample );
      data->feedbackSample = sample;

      // Tick instrument and apply reverb     
      sample = data->volume * data->reverb.tick( sample );
      for ( unsigned int j=0; j<data->nWvOuts; j++ ) data->wvout[j]->tick( sample );
      if ( data->realtime )
        for ( int k=0; k<data->channels; k++ ) *samples++ = sample;
      nTicks--;
    }
    if ( nTicks == 0 ) break;

        if ( data->haveMessage ) 
	{
		// Process control messages.
		processMessage( data );
	}
  }

  return 0;
}

int main( int argc1, char *argv1[] )
{
	int argc = 6;
	char* argv[] = {" ", "-or", "-im","3","-im","2",0};
  TickData data;
  int i;

#if defined(__STK_REALTIME__)
  RtAudio dac;
#endif

  // If you want to change the default sample rate (set in Stk.h), do
  // it before instantiating any objects!  If the sample rate is
  // specified in the command line, it will override this setting.
  Stk::setSampleRate( 44100.0 );

  // By default, warning messages are not printed.  If we want to see
  // them, we need to specify that here.
  Stk::showWarnings( true );

  // Check the command-line arguments for errors and to determine
  // the number of WvOut objects to be instantiated (in utilities.cpp).
  data.nWvOuts = checkArgs( argc, argv );
  data.wvout = (WvOut **) calloc( data.nWvOuts, sizeof(WvOut *) );

  // Parse the command-line flags, instantiate WvOut objects, and
  // instantiate the input message controller (in utilities.cpp).
  try {
    data.realtime = parseArgs( argc, argv, data.wvout, data.messager1 ,data.messager2);
  }
  catch (StkError &) {
    goto cleanup;
  }

	
  // If realtime output, allocate the dac here.
#if defined(__STK_REALTIME__)
  if ( data.realtime ) {
    RtAudioFormat format = ( sizeof(StkFloat) == 8 ) ? RTAUDIO_FLOAT64 : RTAUDIO_FLOAT32;
    RtAudio::StreamParameters parameters;
    parameters.deviceId = dac.getDefaultOutputDevice();
    parameters.nChannels = data.channels;
    unsigned int bufferFrames = RT_BUFFER_SIZE;
    try {
      dac.openStream( &parameters, NULL, format, (unsigned int)Stk::sampleRate(), &bufferFrames, &tick, (void *)&data );
    }
    catch ( RtAudioError& error ) {
      error.printMessage();
      goto cleanup;
    }
  }
#endif

  // Set the reverb parameters.
  data.reverb.setT60( data.t60 );
  data.reverb.setEffectMix( 0.2 );

  // Allocate guitar
  data.guitar = new VoiceWave( nStrings );

  // Configure distortion and feedback.
  data.distortion.setThreshold( 2.0 / 3.0 );
  data.distortion.setA1( 1.0 );
  data.distortion.setA2( 0.0 );
  data.distortion.setA3( -1.0 / 3.0 );
  data.distortionMix = 0.9;
  data.distortionGain = 1.0;
  data.feedbackDelay.setMaximumDelay( (unsigned long int)( 1.1 * Stk::sampleRate() ) );
  data.feedbackDelay.setDelay( 20000 );
  data.feedbackGain = 0.001;
  data.oldFeedbackGain = 0.001;


  // Install an interrupt handler function.
	(void) signal(SIGINT, finish);

  // If realtime output, set our callback function and start the dac.
#if defined(__STK_REALTIME__)
  if ( data.realtime ) {
    try {
      dac.startStream();
    }
    catch ( RtAudioError &error ) {
      error.printMessage();
      goto cleanup;
    }
  }
#endif

  // Setup finished.
  while ( !done ) {
#if defined(__STK_REALTIME__)
    if ( data.realtime )
      // Periodically check "done" status.
      Stk::sleep( 200 );
    else
#endif
      // Call the "tick" function to process data.
      tick( NULL, NULL, 256, 0, 0, (void *)&data );
  }

  // Shut down the output stream.
#if defined(__STK_REALTIME__)
  if ( data.realtime ) {
    try {
      dac.closeStream();
    }
    catch ( RtAudioError& error ) {
      error.printMessage();
    }
  }
#endif

 cleanup:

  for ( i=0; i<(int)data.nWvOuts; i++ ) delete data.wvout[i];
  free( data.wvout );
  delete data.guitar;

	std::cout << "\nStk eguitar finished ... goodbye.\n\n";
  return 0;
}

