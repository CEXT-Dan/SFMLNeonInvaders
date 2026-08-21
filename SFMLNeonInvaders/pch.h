#pragma once

#include "framework.h"
#include "dwmapi.h"

//std
#include <vector>
#include <string>
#include <cmath>
#include <cstdint>
#include <random>
#include <algorithm>
#include <fstream>
#include <map>
#include <optional>
#include <array>
#include <utility>

//SFML headers
#include <SFML/Graphics.hpp>
#include <SFML/Audio.hpp>
#include <SFML/Config.hpp>

// SFML libs
#ifdef _DEBUG
    // SFML Core Static Debug Libraries
#pragma comment(lib, "sfml-main-d.lib")      
#pragma comment(lib, "sfml-system-s-d.lib")  
#pragma comment(lib, "sfml-window-s-d.lib")  
#pragma comment(lib, "sfml-graphics-s-d.lib")
#pragma comment(lib, "sfml-audio-s-d.lib")   

    // Mandatory Sub-System Dependencies (Debug)
#pragma comment(lib, "freetyped.lib")
#pragma comment(lib, "harfbuzzd.lib")
#pragma comment(lib, "FLACd.lib")
#pragma comment(lib, "vorbisd.lib")
#pragma comment(lib, "vorbisencd.lib")
#pragma comment(lib, "vorbisfiled.lib")
#pragma comment(lib, "oggd.lib")
#else
    // SFML Core Static Release Libraries
#pragma comment(lib, "sfml-main.lib")
#pragma comment(lib, "sfml-system-s.lib")   
#pragma comment(lib, "sfml-window-s.lib")   
#pragma comment(lib, "sfml-graphics-s.lib") 
#pragma comment(lib, "sfml-audio-s.lib")    

    // Mandatory Sub-System Dependencies (Release)
#pragma comment(lib, "freetype.lib")
#pragma comment(lib, "harfbuzz.lib")
#pragma comment(lib, "FLAC.lib")
#pragma comment(lib, "vorbis.lib")
#pragma comment(lib, "vorbisenc.lib")
#pragma comment(lib, "vorbisfile.lib")
#pragma comment(lib, "ogg.lib")
#endif

// Win32 GDI & Render Pipeline requirements
#pragma comment(lib, "opengl32.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment( lib, "dwmapi.lib")
