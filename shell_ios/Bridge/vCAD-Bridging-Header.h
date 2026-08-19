// What Swift can see of the C++ application.
//
// Exactly one header, deliberately. Everything Swift needs arrives through `CadViewportView`, so
// this file staying one line is the measure of whether the boundary is holding: a second entry
// here means some part of the shell has started reaching past the bridge.

#import "CadViewport.h"
