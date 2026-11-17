#include "Appearance.h"

#import <AppKit/NSAccessibility.h>
#import <AppKit/NSWorkspace.h>

bool Appearance::reduceMotion()
{
    return [[NSWorkspace sharedWorkspace] accessibilityDisplayShouldReduceMotion];
}
