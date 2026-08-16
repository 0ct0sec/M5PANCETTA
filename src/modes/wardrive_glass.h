#pragma once

#include "wardrive_shared.h"

namespace WardriveScene {

void precomputeGlassBounds();

void glassBounds(int y, int& left, int& right);
int  glassCenterX(int y);
float glassRowTAtY(int y);
bool insideGlass(int x, int y);
int  snapRainXToPane(int x, int y);

} // namespace WardriveScene
