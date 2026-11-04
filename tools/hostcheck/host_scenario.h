/*
===========================================================================

host_scenario.h - what the scenario (shim/switch_shim.cpp) and the fake GPU
(fake_deko3d.cpp) ask of each other.

===========================================================================
*/

#ifndef HOST_SCENARIO_H
#define HOST_SCENARIO_H

#ifdef __cplusplus
extern "C" {
#endif

// is a picture wanted of the frame starting now?
bool		Host_ShotPending( void );
// its name, once: the picture has been asked for
const char *Host_TakeShotName( void );
const char *Host_OutputDir( void );
// the pixel a scenario asked about, or x = -1
void		Host_ProbePixel( int *x, int *y );
// a frame was presented: this is what moves a scenario on
void		Host_FrameStats( int draws, int indexes );
// lines of FAKE DEKO3D ERROR so far
int			FakeDeko3D_Errors( void );

void		Shim_LoadScenario( const char *path, const char *outputDir );

#ifdef __cplusplus
}
#endif

// the console's main(), renamed by the build: it keeps C++ linkage
int			Switch_Main( int argc, char **argv );

#endif // HOST_SCENARIO_H
