/*
 * kmscon - Image Vertex Shader
 *
 * Copyright (c) 2026 the kmscon fork authors
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files
 * (the "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/*
 * Image Vertex Shader
 * A placement's quad (ADR-103), rotated with the display as the cells are.
 */

uniform mat4 projection;
uniform float cos;
uniform float sin;

attribute vec2 position;
attribute vec2 texture_position;

varying vec2 texpos;

vec2 opRotate(in vec2 p)
{
	return p * mat2(vec2(cos, sin), vec2(-sin, cos));
}

void main()
{
	gl_Position = projection * vec4(opRotate(position), 0.0, 1.0);
	texpos = texture_position;
}
