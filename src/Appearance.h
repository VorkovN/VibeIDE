#pragma once

namespace Appearance {
enum class Theme { Light, Dark };

Theme theme();
void setTheme(Theme theme);
}
