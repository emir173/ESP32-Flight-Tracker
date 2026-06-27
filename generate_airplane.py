from PIL import Image, ImageDraw, ImageFilter

w, h = 15, 15
cx, cy = w*2, h*2
outline = (30, 30, 30, 255) # Dark gray

colors = [
    (255, 200, 0, 255),    # 0: Sari (< 15k ft)
    (50, 205, 50, 255),    # 1: Yesil (15k - 25k ft)
    (0, 150, 255, 255),    # 2: Mavi (25k - 35k ft)
    (255, 0, 200, 255)     # 3: Mor/Pembe (> 35k ft)
]

with open("flight_plane_frames.h", "w") as f:
    f.write("#pragma once\n\n")
    f.write("#include <stdint.h>\n\n")
    f.write(f"const uint16_t PLANE_FRAMES_RGB[4][72][{w*h}] = {{\n")
    
    alpha_data = [] # We only need one alpha array since it's the same for all colors
    
    for c_idx, color in enumerate(colors):
        base = Image.new('RGBA', (w*4, h*4), (0,0,0,0))
        d = ImageDraw.Draw(base)

        # Fuselage
        d.line((cx, cy-18, cx, cy+18), fill=color, width=8)
        d.ellipse((cx-4, cy-22, cx+3, cy-10), fill=color) # nose
        d.ellipse((cx-4, cy+10, cx+3, cy+20), fill=color) # tail round

        # Main Wings
        d.polygon([(cx, cy-10), (cx-24, cy-2), (cx-24, cy+4), (cx, cy+0)], fill=color)
        d.polygon([(cx, cy-10), (cx+24, cy-2), (cx+24, cy+4), (cx, cy+0)], fill=color)

        # Tail Wings
        d.polygon([(cx, cy+14), (cx-10, cy+18), (cx-10, cy+22), (cx, cy+20)], fill=color)
        d.polygon([(cx, cy+14), (cx+10, cy+18), (cx+10, cy+22), (cx, cy+20)], fill=color)

        # Engines
        d.ellipse((cx-16, cy-8, cx-6, cy+2), fill=color)
        d.ellipse((cx+6, cy-8, cx+16, cy+2), fill=color)

        # Outline
        outline_base = Image.new('RGBA', (w*4, h*4), (0,0,0,0))
        d_out = ImageDraw.Draw(outline_base)
        d_out.line((cx, cy-18, cx, cy+18), fill=outline, width=12)
        d_out.ellipse((cx-6, cy-24, cx+5, cy-8), fill=outline)
        d_out.ellipse((cx-6, cy+8, cx+5, cy+22), fill=outline)
        d_out.polygon([(cx, cy-12), (cx-26, cy-1), (cx-26, cy+7), (cx, cy+2)], fill=outline)
        d_out.polygon([(cx, cy-12), (cx+26, cy-1), (cx+26, cy+7), (cx, cy+2)], fill=outline)
        d_out.polygon([(cx, cy+12), (cx-12, cy+19), (cx-12, cy+24), (cx, cy+22)], fill=outline)
        d_out.polygon([(cx, cy+12), (cx+12, cy+19), (cx+12, cy+24), (cx, cy+22)], fill=outline)
        d_out.ellipse((cx-18, cy-10, cx-4, cy+4), fill=outline)
        d_out.ellipse((cx+4, cy-10, cx+18, cy+4), fill=outline)

        final_base = Image.alpha_composite(outline_base, base)
        
        f.write("    {\n") # Start color array
        for angle_idx in range(72):
            angle = angle_idx * 5
            rotated = final_base.rotate(-angle, resample=Image.BICUBIC, expand=False)
            downscaled = rotated.resize((w, h), resample=Image.LANCZOS)
            
            rgb_line = []
            alpha_line = []
            for y in range(h):
                for x in range(w):
                    r, g, b, a = downscaled.getpixel((x, y))
                    # BGR swap for TFT_eSPI
                    rgb565 = ((b & 0xF8) << 8) | ((g & 0xFC) << 3) | (r >> 3)
                    rgb_line.append(f"0x{rgb565:04X}")
                    if c_idx == 0:
                        alpha_line.append(str(a))
            
            f.write("        { " + ", ".join(rgb_line) + " },\n")
            if c_idx == 0:
                alpha_data.append("    { " + ", ".join(alpha_line) + " },\n")
        f.write("    },\n") # End color array
        
    f.write("};\n\n")
    
    f.write(f"const uint8_t PLANE_FRAMES_ALPHA[72][{w*h}] = {{\n")
    for a in alpha_data:
        f.write(a)
    f.write("};\n")

print("Generated new passenger jet frames with 4 altitude colors.")
