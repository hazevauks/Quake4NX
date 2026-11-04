/*
===========================================================================

Quake4NX - controller input.

Buttons become the same K_JOY keys openQ4 uses for any gamepad, by position, so
its bindings, weapon wheel, quick chat, menu navigation and prompts work as they
are. These are the cvars the SDL backend owns on other platforms; the settings
menu expects them by name.

	JOY1 L		JOY2 R		JOY3 B (south)	JOY4 A (east)
	JOY5 X (north)	JOY6 Y (west)	JOY7 +		JOY8 -
	JOY9 D-pad up	JOY10 down	JOY11 right	JOY12 left
	JOY13 L stick	JOY14 R stick	JOY15 ZR	JOY16 ZL

===========================================================================
*/

#include "../../idlib/precompiled.h"

#undef BIT
#include <switch.h>

#include "switch_local.h"

static idCVar in_joystick( "in_joystick", "1", CVAR_SYSTEM | CVAR_ARCHIVE | CVAR_BOOL, "enable controller input" );
static idCVar in_gamepadFamily( "in_gamepadFamily", "nintendo", CVAR_SYSTEM | CVAR_ROM | CVAR_NOCHEAT, "the controller family, which decides menus' confirm button and prompts" );
static idCVar in_promptStyle( "in_promptStyle", "3", CVAR_SYSTEM | CVAR_ARCHIVE | CVAR_INTEGER, "controller prompt glyphs: 0 the controller's own, 1 Xbox, 2 PlayStation, 3 Nintendo, 4 Steam Deck, 5 generic", 0, 5 );
static idCVar in_joystickDeadZone( "in_joystickDeadZone", "0.15", CVAR_SYSTEM | CVAR_ARCHIVE | CVAR_FLOAT, "stick dead zone", 0.0f, 0.95f );
static idCVar in_joystickTriggerThreshold( "in_joystickTriggerThreshold", "0.35", CVAR_SYSTEM | CVAR_ARCHIVE | CVAR_FLOAT, "unused: the Switch's triggers are digital", 0.0f, 1.0f );
static idCVar in_joystickLookSensitivity( "in_joystickLookSensitivity", "0.75", CVAR_SYSTEM | CVAR_ARCHIVE | CVAR_FLOAT, "look sensitivity scale", 0.1f, 4.0f );
static idCVar in_joystickLookCurve( "in_joystickLookCurve", "1.35", CVAR_SYSTEM | CVAR_ARCHIVE | CVAR_FLOAT, "look response curve", 1.0f, 3.0f );
static idCVar in_joystickMoveCurve( "in_joystickMoveCurve", "1.0", CVAR_SYSTEM | CVAR_ARCHIVE | CVAR_FLOAT, "movement response curve", 1.0f, 3.0f );
static idCVar in_joystickInvertLook( "in_joystickInvertLook", "0", CVAR_SYSTEM | CVAR_ARCHIVE | CVAR_BOOL, "invert look pitch" );
static idCVar in_joystickSouthpaw( "in_joystickSouthpaw", "0", CVAR_SYSTEM | CVAR_ARCHIVE | CVAR_BOOL, "swap the movement and look sticks" );
static idCVar in_joystickRumble( "in_joystickRumble", "1", CVAR_SYSTEM | CVAR_ARCHIVE | CVAR_BOOL, "enable rumble" );
static idCVar in_joystickRumbleScale( "in_joystickRumbleScale", "1.0", CVAR_SYSTEM | CVAR_ARCHIVE | CVAR_FLOAT, "rumble strength", 0.0f, 2.0f );
static idCVar in_gyro( "in_gyro", "0", CVAR_SYSTEM | CVAR_ARCHIVE | CVAR_BOOL, "aim with the controller's gyro" );
static idCVar in_gyroSensitivity( "in_gyroSensitivity", "1.0", CVAR_SYSTEM | CVAR_ARCHIVE | CVAR_FLOAT, "gyro aim sensitivity", 0.0f, 8.0f );
static idCVar in_gyroDeadZone( "in_gyroDeadZone", "0.015", CVAR_SYSTEM | CVAR_ARCHIVE | CVAR_FLOAT, "ignore gyro rates below this, in turns per second", 0.0f, 2.0f );
static idCVar in_gyroInvertYaw( "in_gyroInvertYaw", "0", CVAR_SYSTEM | CVAR_ARCHIVE | CVAR_BOOL, "invert gyro yaw" );
static idCVar in_gyroInvertPitch( "in_gyroInvertPitch", "0", CVAR_SYSTEM | CVAR_ARCHIVE | CVAR_BOOL, "invert gyro pitch" );
static idCVar in_touchscreen( "in_touchscreen", "1", CVAR_SYSTEM | CVAR_ARCHIVE | CVAR_BOOL, "the touch screen drives the menus" );
static idCVar in_mouse( "in_mouse", "1", CVAR_SYSTEM | CVAR_ARCHIVE | CVAR_BOOL, "unused: no mouse on the Switch" );

static const int SCREEN_WIDTH_PIXELS	= 1280;
static const int SCREEN_HEIGHT_PIXELS	= 720;
static const float GYRO_TURNS_TO_MOUSE	= 2200.0f;	// mouse counts per turn at sensitivity 1
static const float RUMBLE_FREQ_LOW		= 160.0f;
static const float RUMBLE_FREQ_HIGH		= 320.0f;

typedef struct {
	u64		button;
	int		key;
} buttonMap_t;

// labels for these keys are in ui/UserInterfaceRetained.cpp, by position
static const buttonMap_t s_buttonMap[] = {
	{ HidNpadButton_L,		K_JOY1 },
	{ HidNpadButton_R,		K_JOY2 },
	{ HidNpadButton_B,		K_JOY3 },
	{ HidNpadButton_A,		K_JOY4 },
	{ HidNpadButton_X,		K_JOY5 },
	{ HidNpadButton_Y,		K_JOY6 },
	{ HidNpadButton_Plus,	K_JOY7 },
	{ HidNpadButton_Minus,	K_JOY8 },
	{ HidNpadButton_Up,		K_JOY9 },
	{ HidNpadButton_Down,	K_JOY10 },
	{ HidNpadButton_Right,	K_JOY11 },
	{ HidNpadButton_Left,	K_JOY12 },
	{ HidNpadButton_StickL,	K_JOY13 },
	{ HidNpadButton_StickR,	K_JOY14 },
	{ HidNpadButton_ZR,		K_JOY15 },
	{ HidNpadButton_ZL,		K_JOY16 },
};
static const int NUM_BUTTONS = sizeof( s_buttonMap ) / sizeof( s_buttonMap[0] );

static const int QUEUE_SIZE = 256;
static const int QUEUE_MASK = QUEUE_SIZE - 1;

typedef struct {
	int		key;
	bool	down;
} keyEvent_t;

typedef struct {
	int		axis;
	int		value;
} axisEvent_t;

static keyEvent_t	s_keyQueue[QUEUE_SIZE];
static int			s_keyHead = 0;
static int			s_keyTail = 0;
static keyEvent_t	s_polledKeys[QUEUE_SIZE];
static int			s_polledKeyCount = 0;

static int			s_axes[MAX_JOYSTICK_AXIS];
static axisEvent_t	s_polledAxes[MAX_JOYSTICK_AXIS];
static int			s_polledAxisCount = 0;

static bool			s_inputReady = false;
static PadState		s_pad;
static u64			s_buttonsHeld = 0;		// last frame's, to see what was just pressed
static u64			s_keysPosted = 0;		// buttons whose key is down in the engine
static bool			s_touchDown = false;
static int			s_touchX = 0;
static int			s_touchY = 0;
static bool			s_consoleCombo = false;

static HidSixAxisSensorHandle	s_sixAxis[4];
static bool			s_gyroStarted = false;
static float		s_gyroRestX = 0.0f;
static float		s_gyroRestY = 0.0f;
static u64			s_gyroLastTick = 0;

static HidVibrationDeviceHandle	s_vibration[4];
static bool			s_vibrationReady = false;
static u64			s_rumbleUntilTick = 0;
static bool			s_rumbling = false;

bool RetainedUI_IsOpen();

static bool Switch_MenuActive( void ) {
	return RetainedUI_IsOpen() || ( session != NULL && session->GetActiveGUI() != NULL );
}

/*
================
Key events

Posted twice, as the SDL backend does: the event queue feeds the menus and the
console, the polled queue feeds the game's bindings.
================
*/
static void Switch_PostKey( int key, bool down ) {
	Switch_QueueEvent( SE_KEY, key, down ? 1 : 0, 0, NULL );

	Sys_EnterCriticalSection( CRITICAL_SECTION_ONE );
	const int next = ( s_keyHead + 1 ) & QUEUE_MASK;
	if ( next == s_keyTail ) {
		s_keyTail = ( s_keyTail + 1 ) & QUEUE_MASK;
	}
	s_keyQueue[s_keyHead].key = key;
	s_keyQueue[s_keyHead].down = down;
	s_keyHead = next;
	Sys_LeaveCriticalSection( CRITICAL_SECTION_ONE );
}

static void Switch_ToggleConsole( void ) {
	Switch_QueueEvent( SE_KEY, Sys_GetConsoleKey( false ), 1, 0, NULL );
	Switch_QueueEvent( SE_KEY, Sys_GetConsoleKey( false ), 0, 0, NULL );
}

/*
================
System keyboard
================
*/
static bool Switch_ShowKeyboard( const char *header, char *text, size_t textSize ) {
	SwkbdConfig kbd;
	text[0] = '\0';
	if ( R_FAILED( swkbdCreate( &kbd, 0 ) ) ) {
		return false;
	}
	swkbdConfigMakePresetDefault( &kbd );
	swkbdConfigSetHeaderText( &kbd, header );
	const Result rc = swkbdShow( &kbd, text, textSize );
	swkbdClose( &kbd );
	return R_SUCCEEDED( rc ) && text[0] != '\0';
}

static void Switch_TypeText( const char *text ) {
	for ( const char *c = text; *c; c++ ) {
		const unsigned char ch = (unsigned char)*c;
		if ( ch >= 32 && ch < 127 ) {
			Switch_QueueEvent( SE_CHAR, ch, 0, 0, NULL );
		}
	}
}

static void Switch_ConsoleTextEntry( void ) {
	char text[256];
	if ( Switch_ShowKeyboard( "Quake 4 console", text, sizeof( text ) ) ) {
		Switch_TypeText( text );
		Switch_PostKey( K_ENTER, true );
		Switch_PostKey( K_ENTER, false );
	}
}

/*
================
Buttons
================
*/
static void Switch_ReleaseButtons( void ) {
	for ( int i = 0; i < NUM_BUTTONS; i++ ) {
		if ( s_keysPosted & s_buttonMap[i].button ) {
			Switch_PostKey( s_buttonMap[i].key, false );
		}
	}
	s_keysPosted = 0;
}

static void Switch_TapKey( int key ) {
	Switch_PostKey( key, true );
	Switch_PostKey( key, false );
}

static void Switch_UpdateButtons( u64 held ) {
	const u64 pressed = held & ~s_buttonsHeld;
	s_buttonsHeld = held;

	// + and - together open the console from anywhere
	const u64 combo = HidNpadButton_Plus | HidNpadButton_Minus;
	if ( ( held & combo ) == combo ) {
		if ( !s_consoleCombo ) {
			s_consoleCombo = true;
			Switch_ReleaseButtons();
			Switch_ToggleConsole();
		}
		return;
	}
	s_consoleCombo = false;

	// with the console down, A types a command, the D-pad walks its history, L and R scroll it
	if ( console && console->Active() ) {
		Switch_ReleaseButtons();
		if ( pressed & HidNpadButton_A ) {
			Switch_ConsoleTextEntry();
		}
		if ( pressed & HidNpadButton_B ) {
			Switch_ToggleConsole();
		}
		if ( pressed & HidNpadButton_Up ) {
			Switch_TapKey( K_UPARROW );
		}
		if ( pressed & HidNpadButton_Down ) {
			Switch_TapKey( K_DOWNARROW );
		}
		if ( pressed & HidNpadButton_L ) {
			Switch_TapKey( K_PGUP );
		}
		if ( pressed & HidNpadButton_R ) {
			Switch_TapKey( K_PGDN );
		}
		return;
	}

	for ( int i = 0; i < NUM_BUTTONS; i++ ) {
		const u64 button = s_buttonMap[i].button;
		if ( ( pressed & button ) && !( s_keysPosted & button ) ) {
			Switch_PostKey( s_buttonMap[i].key, true );
			s_keysPosted |= button;
		} else if ( !( held & button ) && ( s_keysPosted & button ) ) {
			Switch_PostKey( s_buttonMap[i].key, false );
			s_keysPosted &= ~button;
		}
	}
}

/*
================
Sticks
================
*/
static void Switch_NormalizeStick( const HidAnalogStickState &stick, float deadZone, float curve, int &outX, int &outY ) {
	const float x = stick.x / (float)JOYSTICK_MAX;
	const float y = stick.y / (float)JOYSTICK_MAX;
	const float length = idMath::Sqrt( x * x + y * y );
	const float dead = idMath::ClampFloat( 0.0f, 0.95f, deadZone );
	if ( length <= dead ) {
		outX = outY = 0;
		return;
	}
	float adjusted = ( Min( length, 1.0f ) - dead ) / ( 1.0f - dead );
	adjusted = idMath::ClampFloat( 0.0f, 1.0f, adjusted );
	const float response = idMath::ClampFloat( 1.0f, 3.0f, curve );
	if ( response != 1.0f && adjusted > 0.0f ) {
		adjusted = idMath::Pow( adjusted, response );
	}
	const float scale = adjusted / length;
	outX = idMath::ClampInt( -127, 127, (int)( x * scale * 127.0f ) );
	outY = idMath::ClampInt( -127, 127, (int)( y * scale * 127.0f ) );
}

static void Switch_UpdateAxes( const HidAnalogStickState &left, const HidAnalogStickState &right ) {
	const bool southpaw = in_joystickSouthpaw.GetBool();
	const HidAnalogStickState &moveStick = southpaw ? right : left;
	const HidAnalogStickState &lookStick = southpaw ? left : right;

	cvarSystem->SetCVarFloat( "ui_radialStickX", lookStick.x / (float)JOYSTICK_MAX );
	cvarSystem->SetCVarFloat( "ui_radialStickY", -lookStick.y / (float)JOYSTICK_MAX );

	const float deadZone = in_joystickDeadZone.GetFloat();
	int moveX, moveY, lookX, lookY;
	Switch_NormalizeStick( moveStick, deadZone, in_joystickMoveCurve.GetFloat(), moveX, moveY );
	Switch_NormalizeStick( lookStick, deadZone, in_joystickLookCurve.GetFloat(), lookX, lookY );
	// HID's y grows upwards, as the look axis expects it. The move axis is the other way.
	lookY = -lookY;
	if ( in_joystickInvertLook.GetBool() ) {
		lookY = -lookY;
	}

	Sys_EnterCriticalSection( CRITICAL_SECTION_ONE );
	s_axes[AXIS_SIDE] = lookX;
	s_axes[AXIS_FORWARD] = lookY;
	s_axes[AXIS_UP] = 0;
	s_axes[AXIS_ROLL] = 127;	// dedicated look axes are present (framework/UsercmdGen.cpp)
	s_axes[AXIS_YAW] = moveX;
	s_axes[AXIS_PITCH] = moveY;
	Sys_LeaveCriticalSection( CRITICAL_SECTION_ONE );
}

/*
================
Gyro

Rates in turns per second become mouse movement, while the game has the input.
================
*/
static void Switch_StartGyro( void ) {
	if ( s_gyroStarted ) {
		return;
	}
	hidGetSixAxisSensorHandles( &s_sixAxis[0], 1, HidNpadIdType_Handheld, HidNpadStyleTag_NpadHandheld );
	hidGetSixAxisSensorHandles( &s_sixAxis[1], 1, HidNpadIdType_No1, HidNpadStyleTag_NpadFullKey );
	hidGetSixAxisSensorHandles( &s_sixAxis[2], 2, HidNpadIdType_No1, HidNpadStyleTag_NpadJoyDual );
	for ( int i = 0; i < 4; i++ ) {
		hidStartSixAxisSensor( s_sixAxis[i] );
	}
	s_gyroStarted = true;
	s_gyroLastTick = armGetSystemTick();
}

static void Switch_UpdateGyro( void ) {
	if ( !in_gyro.GetBool() ) {
		return;
	}
	Switch_StartGyro();

	const u64 now = armGetSystemTick();
	const float dt = (float)armTicksToNs( now - s_gyroLastTick ) * 1e-9f;
	s_gyroLastTick = now;

	const u32 style = padGetStyleSet( &s_pad );
	int handle = 1;
	if ( style & HidNpadStyleTag_NpadHandheld ) {
		handle = 0;
	} else if ( style & HidNpadStyleTag_NpadJoyDual ) {
		handle = 3;	// the right Joy-Con, in the hand that aims
	}
	HidSixAxisSensorState state = {};
	if ( !hidGetSixAxisSensorStates( s_sixAxis[handle], &state, 1 ) ) {
		return;
	}

	const float dead = in_gyroDeadZone.GetFloat();
	float yaw = idMath::Fabs( state.angular_velocity.z ) > dead ? -state.angular_velocity.z : 0.0f;
	float pitch = idMath::Fabs( state.angular_velocity.x ) > dead ? -state.angular_velocity.x : 0.0f;
	if ( in_gyroInvertYaw.GetBool() ) {
		yaw = -yaw;
	}
	if ( in_gyroInvertPitch.GetBool() ) {
		pitch = -pitch;
	}
	const float scale = in_gyroSensitivity.GetFloat() * GYRO_TURNS_TO_MOUSE * dt;
	s_gyroRestX += yaw * scale;
	s_gyroRestY += pitch * scale;
	const int dx = (int)s_gyroRestX;
	const int dy = (int)s_gyroRestY;
	s_gyroRestX -= dx;
	s_gyroRestY -= dy;
	if ( dx || dy ) {
		Switch_QueueEvent( SE_MOUSE, dx, dy, 0, NULL );
	}
}

/*
================
Touch screen

Drags move the menu cursor, and a touch is its click. In play it does nothing.
================
*/
static void Switch_UpdateTouch( void ) {
	HidTouchScreenState touch = {};
	const bool touching = in_touchscreen.GetBool() && hidGetTouchScreenStates( &touch, 1 ) && touch.count > 0;
	const bool inMenu = Switch_MenuActive();

	if ( touching && inMenu ) {
		const int x = touch.touches[0].x;
		const int y = touch.touches[0].y;
		if ( s_touchDown ) {
			const int dx = ( x - s_touchX ) * engineWindowState.vidWidth / SCREEN_WIDTH_PIXELS;
			const int dy = ( y - s_touchY ) * engineWindowState.vidHeight / SCREEN_HEIGHT_PIXELS;
			if ( dx || dy ) {
				Switch_QueueEvent( SE_MOUSE, dx, dy, 0, NULL );
			}
		}
		s_touchX = x;
		s_touchY = y;
	}
	// a click starts only inside a menu; the release always comes through
	if ( touching != s_touchDown && ( !touching || inMenu ) ) {
		Switch_PostKey( K_MOUSE1, touching );
		s_touchDown = touching;
	}
}

/*
================
Rumble
================
*/
static void Switch_StopRumble( void ) {
	if ( !s_vibrationReady || !s_rumbling ) {
		return;
	}
	HidVibrationValue values[2];
	for ( int i = 0; i < 2; i++ ) {
		values[i].amp_low = 0.0f;
		values[i].freq_low = RUMBLE_FREQ_LOW;
		values[i].amp_high = 0.0f;
		values[i].freq_high = RUMBLE_FREQ_HIGH;
	}
	hidSendVibrationValues( &s_vibration[0], values, 2 );
	hidSendVibrationValues( &s_vibration[2], values, 2 );
	s_rumbling = false;
}

bool Sys_SetJoystickRumble( float lowFrequency, float highFrequency, int durationMsec ) {
	if ( !in_joystick.GetBool() || !in_joystickRumble.GetBool() || durationMsec <= 0 ) {
		Switch_StopRumble();
		return false;
	}
	if ( !s_vibrationReady ) {
		return false;
	}
	const float scale = in_joystickRumbleScale.GetFloat();
	HidVibrationValue values[2];
	for ( int i = 0; i < 2; i++ ) {
		values[i].amp_low = idMath::ClampFloat( 0.0f, 1.0f, lowFrequency * scale );
		values[i].freq_low = RUMBLE_FREQ_LOW;
		values[i].amp_high = idMath::ClampFloat( 0.0f, 1.0f, highFrequency * scale );
		values[i].freq_high = RUMBLE_FREQ_HIGH;
	}
	const u32 style = padGetStyleSet( &s_pad );
	hidSendVibrationValues( ( style & HidNpadStyleTag_NpadHandheld ) ? &s_vibration[0] : &s_vibration[2], values, 2 );
	s_rumbling = true;
	s_rumbleUntilTick = armGetSystemTick() + armNsToTicks( (u64)durationMsec * 1000000ULL );
	return true;
}

/*
================
Per-frame
================
*/
void Switch_PollInput( void ) {
	if ( !s_inputReady ) {
		return;
	}

	padUpdate( &s_pad );

	if ( s_rumbling && armGetSystemTick() > s_rumbleUntilTick ) {
		Switch_StopRumble();
	}

	if ( !in_joystick.GetBool() ) {
		memset( s_axes, 0, sizeof( s_axes ) );
		return;
	}

	Switch_UpdateButtons( padGetButtons( &s_pad ) );
	Switch_UpdateAxes( padGetStickPos( &s_pad, 0 ), padGetStickPos( &s_pad, 1 ) );
	Switch_UpdateTouch();
	if ( !Switch_MenuActive() ) {
		Switch_UpdateGyro();
	}
}

/*
================
Switch_ApplyDefaultBinds

openQ4's default.cfg holds the button layout. On the first start, the D-pad's sides move through the weapons
the way they point, and the text is sized for a small screen. Only once: the player may change them later.
================
*/
static idCVar in_defaultBindsSet( "in_defaultBindsSet", "0", CVAR_SYSTEM | CVAR_ARCHIVE | CVAR_INTEGER | CVAR_NOCHEAT, "the controller scheme the settings were last set up for (internal)" );
static const int SWITCH_CONTROL_SCHEME = 1;

void Switch_ApplyDefaultBinds( void ) {
	if ( in_defaultBindsSet.GetInteger() >= SWITCH_CONTROL_SCHEME ) {
		return;
	}
	idKeyInput::SetBinding( K_JOY11, "_impulse14" );	// D-pad right: next weapon
	idKeyInput::SetBinding( K_JOY12, "_impulse15" );	// D-pad left: previous weapon
	cvarSystem->SetCVarFloat( "ui_retainedScale", 1.5f );
	cvarSystem->SetCVarFloat( "ui_retainedTextScale", 1.25f );
	cvarSystem->SetCVarFloat( "ui_hudScale", 1.25f );
	in_defaultBindsSet.SetInteger( SWITCH_CONTROL_SCHEME );
}

/*
================
System interface
================
*/
void Sys_InitInput( void ) {
	padConfigureInput( 1, HidNpadStyleSet_NpadStandard );
	padInitializeDefault( &s_pad );
	hidInitializeTouchScreen();

	hidInitializeVibrationDevices( &s_vibration[0], 2, HidNpadIdType_Handheld, HidNpadStyleTag_NpadHandheld );
	hidInitializeVibrationDevices( &s_vibration[2], 2, HidNpadIdType_No1, HidNpadStyleTag_NpadJoyDual );
	s_vibrationReady = true;

	memset( s_axes, 0, sizeof( s_axes ) );
	s_buttonsHeld = 0;
	s_keysPosted = 0;
	s_touchDown = false;
	s_inputReady = true;
}

void Sys_ShutdownInput( void ) {
	Switch_StopRumble();
	s_inputReady = false;
}

void Sys_ClearInputEvents( void ) {
	Sys_EnterCriticalSection( CRITICAL_SECTION_ONE );
	s_keyHead = s_keyTail = 0;
	s_polledKeyCount = 0;
	s_polledAxisCount = 0;
	Sys_LeaveCriticalSection( CRITICAL_SECTION_ONE );
}

void Sys_InitScanTable( void ) {
}

const unsigned char *Sys_GetScanTable( void ) {
	static const unsigned char table[256] = { 0 };
	return table;
}

unsigned char Sys_GetConsoleKey( bool shifted ) {
	return shifted ? '~' : '`';
}

unsigned char Sys_MapCharForKey( int key ) {
	return (unsigned char)key;
}

int Sys_PollKeyboardInputEvents( void ) {
	Sys_EnterCriticalSection( CRITICAL_SECTION_ONE );
	s_polledKeyCount = 0;
	while ( s_keyTail != s_keyHead ) {
		s_polledKeys[s_polledKeyCount++] = s_keyQueue[s_keyTail];
		s_keyTail = ( s_keyTail + 1 ) & QUEUE_MASK;
	}
	Sys_LeaveCriticalSection( CRITICAL_SECTION_ONE );
	return s_polledKeyCount;
}

int Sys_ReturnKeyboardInputEvent( const int n, int &ch, bool &state ) {
	if ( n < 0 || n >= s_polledKeyCount ) {
		ch = 0;
		state = false;
		return 0;
	}
	ch = s_polledKeys[n].key;
	state = s_polledKeys[n].down;
	return ch;
}

void Sys_EndKeyboardInputEvents( void ) {
}

int Sys_PollMouseInputEvents( void ) {
	return 0;
}

int Sys_ReturnMouseInputEvent( const int n, int &action, int &value ) {
	action = 0;
	value = 0;
	return 0;
}

void Sys_EndMouseInputEvents( void ) {
}

int Sys_PollJoystickInputEvents( void ) {
	Sys_EnterCriticalSection( CRITICAL_SECTION_ONE );
	s_polledAxisCount = 0;
	for ( int axis = 0; axis < MAX_JOYSTICK_AXIS; axis++ ) {
		s_polledAxes[s_polledAxisCount].axis = axis;
		s_polledAxes[s_polledAxisCount].value = s_axes[axis];
		s_polledAxisCount++;
	}
	Sys_LeaveCriticalSection( CRITICAL_SECTION_ONE );
	return s_polledAxisCount;
}

int Sys_ReturnJoystickInputEvent( const int n, int &axis, int &value ) {
	if ( n < 0 || n >= s_polledAxisCount ) {
		axis = 0;
		value = 0;
		return 0;
	}
	axis = s_polledAxes[n].axis;
	value = s_polledAxes[n].value;
	return 1;
}

void Sys_EndJoystickInputEvents( void ) {
}

bool Sys_GetJoystickAxisState( int axis, int &value ) {
	if ( !in_joystick.GetBool() || axis < 0 || axis >= MAX_JOYSTICK_AXIS ) {
		value = 0;
		return false;
	}
	Sys_EnterCriticalSection( CRITICAL_SECTION_ONE );
	value = s_axes[axis];
	Sys_LeaveCriticalSection( CRITICAL_SECTION_ONE );
	return true;
}

// VR's entry points, which the shared code calls through
void Sys_PostVRControllerKey( int key, bool down ) {
}

bool Sys_PostVRPointer( bool onScreen, float u, float v ) {
	return false;
}

/*
================
Sys_SDL_GamepadGlyphFamily

What the SDL backend answers for the prompts: a Nintendo controller, with B south,
A east, Y west and X north.
================
*/
int Sys_SDL_GamepadGlyphFamily( int faceLabels[4] ) {
	static const int nintendo[4] = { 2, 1, 4, 3 };
	static const int xbox[4] = { 1, 2, 3, 4 };
	static const int playstation[4] = { 5, 6, 7, 8 };
	static const int unknown[4] = { 0, 0, 0, 0 };
	int family = in_promptStyle.GetInteger();
	if ( family == 0 ) {
		family = 3;
	}
	const int *labels = family == 2 ? playstation : family == 3 ? nintendo : family == 1 || family == 4 ? xbox : unknown;
	for ( int i = 0; i < 4; i++ ) {
		faceLabels[i] = labels[i];
	}
	return family == 5 ? 0 : family;
}
