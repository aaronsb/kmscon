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
 * of the alpha atlas bound to deco, at the cells decopos.y and decopos.z name
 * (a blank one for none), each tinted with its own colour and repeated in
 * every cell of a wide glyph; decopos.x is the x within the quad's cells,
 * small enough to stay exact under a 16-bit mediump. A frame without the
 * sprites has decorated 0. Upstream samples one alpha atlas and tints it; this
 * departure is recorded in ADR-102's corrections from step 6 (ADR-303).
 *
 * layer (ADR-103): 0 draws the cell whole and opaque. A frame with image
 * placements draws its cells twice, with pictures between: 1 draws only a
 * background that is not the default (bgcol.a 1), 2 only the text layer,
 * premultiplied, for blending over what is under it.
 */

precision mediump float;

uniform sampler2D atlas;
uniform sampler2D deco;
uniform bool colour;
uniform float advance_htex;
uniform float advance_vtex;
uniform float deco_htex;
uniform float deco_vtex;
uniform float decorated;
uniform int layer;

varying vec2 texpos;
varying vec3 fgcol;
varying vec4 bgcol;
varying vec3 ulcol;
varying vec3 stcol;
varying vec3 decopos;

void main()
{
	if (layer == 1) {
		if (bgcol.a < 0.5)
			discard;
		gl_FragColor = vec4(bgcol.rgb, 1.0);
		return;
	}

	vec2 pos = vec2(texpos.x * advance_htex, texpos.y * advance_vtex);
	vec4 tex = texture2D(atlas, pos);
	vec4 glyph = colour ? tex : vec4(tex.a * fgcol, tex.a);
	float x = fract(decopos.x);
	float y = texpos.y * deco_vtex;
	float u = texture2D(deco, vec2((decopos.y + x) * deco_htex, y)).a * decorated;
	float s = texture2D(deco, vec2((decopos.z + x) * deco_htex, y)).a * decorated;

	/* the text layer, premultiplied: underline, glyph, strike-through */
	vec4 text = vec4(u * ulcol, u);
	text = glyph + (1.0 - glyph.a) * text;
	text = vec4(s * stcol, s) + (1.0 - s) * text;
	if (layer == 2)
		gl_FragColor = text;
	else
		gl_FragColor = vec4(text.rgb + (1.0 - text.a) * bgcol.rgb, 1.0);
}
