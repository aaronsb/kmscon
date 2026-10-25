/*
 * kmscon - Fragment Shader
 *
 * Copyright (c) 2011-2012 David Herrmann <dh.herrmann@googlemail.com>
 * Copyright (c) 2011 University of Tuebingen
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
 * Fragment Shader
 * A basic fragment shader which applies a 2D texture and blends foreground and
 * background colors.
 *
 * kmscon fork (ADR-102 step 6): a cell is its underline, then its glyph, then
 * its strike-through, over its background, as bbulk composes it. The glyph
 * comes from an alpha atlas, tinted with the foreground, or from an RGBA atlas
 * (colour set), premultiplied and untinted. The decoration sprites are cells
 * of the alpha atlas bound to deco, at the cells decocell names (a blank one
 * for none), each tinted with its own colour and repeated in every cell of a
 * wide glyph. Upstream samples one alpha atlas and tints it; this departure is
 * recorded in the PR that made it (ADR-303).
 */

precision mediump float;

uniform sampler2D atlas;
uniform sampler2D deco;
uniform bool colour;
uniform float advance_htex;
uniform float advance_vtex;
uniform float deco_htex;
uniform float deco_vtex;

varying vec2 texpos;
varying vec3 fgcol;
varying vec3 bgcol;
varying vec3 ulcol;
varying vec3 stcol;
varying vec2 decocell;

void main()
{
	vec2 pos = vec2(texpos.x * advance_htex, texpos.y * advance_vtex);
	vec4 tex = texture2D(atlas, pos);
	vec4 glyph = colour ? tex : vec4(tex.a * fgcol, tex.a);
	float x = fract(texpos.x);
	float y = texpos.y * deco_vtex;
	float u = texture2D(deco, vec2((decocell.x + x) * deco_htex, y)).a;
	float s = texture2D(deco, vec2((decocell.y + x) * deco_htex, y)).a;

	/* the text layer, premultiplied: underline, glyph, strike-through */
	vec4 text = vec4(u * ulcol, u);
	text = glyph + (1.0 - glyph.a) * text;
	text = vec4(s * stcol, s) + (1.0 - s) * text;
	gl_FragColor = vec4(text.rgb + (1.0 - text.a) * bgcol, 1.0);
}
