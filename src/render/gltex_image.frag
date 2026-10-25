/*
 * kmscon - Image Fragment Shader
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
 * Image Fragment Shader
 * A placement's picture (ADR-103), premultiplied ARGB32 uploaded as the bytes
 * it is in memory, which GL_RGBA reads as B, G, R, A: swizzled back here. It
 * is sampled within clip (u0, v0, u1, v1), its source rectangle half a texel
 * in, so bilinear filtering takes nothing from the pixels beside it. The
 * precision line comes before this source (gltex.c, image_shader_new).
 */

uniform sampler2D picture;
uniform vec4 clip;

varying vec2 texpos;

void main()
{
	gl_FragColor = texture2D(picture, clamp(texpos, clip.xy, clip.zw)).bgra;
}
