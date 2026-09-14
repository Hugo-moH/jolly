/*
MIT License

Copyright (c) 2020-2022 Rupert Carmichael

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#version 450

layout(set = 0, binding = 0) uniform sampler2D source;

layout(push_constant) uniform Push {
    vec4 sourceSize;
    vec4 targetSize;
    int masktype;
    float maskstr;
    float scanstr;
    float sharpness;
    float curve;
    float corner;
    float tcurve;
} pc;

layout(location = 0) in vec2 texCoord;
layout(location = 0) out vec4 fragColor;

#define maskstr 0.5

void main() {
    // Find out where in the LCD screen we are
    vec2 lcdCoord = texCoord * pc.sourceSize.xy * 3.0;

    // Find out which column of the pixel we're in
    float col = floor(mod(lcdCoord.x, 3.0));

    // Shade all pixels
    vec4 shade = vec4(maskstr, maskstr, maskstr, 1.0);

    // Shade stronger in red, green, or blue for each column
    if (col == 1.0)
        shade.g = 1.0;
    else if (col == 2.0)
        shade.b = 1.0;
    else
        shade.r = 1.0;

    // Draw a horizontal scanline at the bottom of each pixel
    if (floor(mod(lcdCoord.y, 3.0)) == 0.0)
        shade.rgba = vec4(maskstr, maskstr, maskstr, 1.0);

    fragColor = texture(source, texCoord) * shade;
}
