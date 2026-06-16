#!/usr/bin/env python3

import sys
import os
sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".venv", "Lib", "site-packages"))

import re
import matplotlib.pyplot as plt
import matplotlib.patches as patches
from matplotlib.patches import Circle, Rectangle, Polygon, FancyBboxPatch
import numpy as np

def parse_pcb_file(filename):
    with open(filename, 'r') as f:
        content = f.read()
    return content

def extract_footprints(pcb_content):
    footprints = []
    pattern = r'\(footprint\s+"([^"]+)".*?\(property\s+"Reference"\s+\(at\s+([\d.-]+)\s+([\d.-]+)\).*?\(property\s+"Value"\s+\(at\s+([\d.-]+)\s+([\d.-]+)\).*?\(pad\s+"(\d+)".*?\(at\s+([\d.-]+)\s+([\d.-]+)\).*?\(size\s+([\d.-]+)\s+([\d.-]+)\)'
    
    footprint_pattern = r'\(footprint\s+"([^"]+)"'
    matches = re.finditer(footprint_pattern, pcb_content)
    
    for match in matches:
        ref = match.group(1)
        footprints.append({'ref': ref, 'pos': (0, 0), 'pads': []})
    
    return footprints

def visualize_pcb(pcb_filename, output_image=None):
    fig, ax = plt.subplots(1, 1, figsize=(16, 12), dpi=100)
    
    board_x_min, board_y_min = 5, 5
    board_x_max, board_y_max = 95, 75
    
    board_outline = Rectangle((board_x_min, board_y_min), 
                              board_x_max - board_x_min, 
                              board_y_max - board_y_min,
                              linewidth=3, edgecolor='black', 
                              facecolor='lightgray', alpha=0.3)
    ax.add_patch(board_outline)
    
    mounting_hole_positions = [(8, 8), (92, 8), (8, 72), (92, 72)]
    for x, y in mounting_hole_positions:
        circle = Circle((x, y), 1.6, edgecolor='black', 
                       facecolor='none', linewidth=2, linestyle='--')
        ax.add_patch(circle)
        ax.text(x, y-2.5, 'MH', ha='center', fontsize=8, weight='bold')
    
    esp32_headers = [
        {'name': 'ESP32_L', 'x': 15, 'y': 40, 'pins': 16, 'orientation': 'v'},
        {'name': 'ESP32_R', 'x': 37.86, 'y': 40, 'pins': 16, 'orientation': 'v'},
    ]
    
    for header in esp32_headers:
        x, y = header['x'], header['y']
        pitch = 2.54
        n_pins = header['pins']
        
        if header['orientation'] == 'v':
            for i in range(n_pins):
                pin_y = y - (n_pins - 1) * pitch / 2 + i * pitch
                circle = Circle((x, pin_y), 0.4, edgecolor='red', 
                               facecolor='white', linewidth=1.5)
                ax.add_patch(circle)
        
        ax.text(x, y - (n_pins + 2) * pitch / 2, header['name'], 
               ha='center', fontsize=10, weight='bold', color='red')
    
    connectors = [
        {'name': 'GY45_MMA8452', 'x': 75, 'y': 25, 'pins': 4, 'color': 'blue'},
        {'name': 'JSN_SR04T', 'x': 75, 'y': 35, 'pins': 4, 'color': 'blue'},
        {'name': 'TP4056_MODULE', 'x': 75, 'y': 50, 'pins': 6, 'color': 'green'},
        {'name': 'BOOST_5V_MODULE', 'x': 75, 'y': 65, 'pins': 4, 'color': 'green'},
        {'name': '18650_BAT', 'x': 20, 'y': 65, 'pins': 2, 'color': 'orange'},
        {'name': 'POWER_SW', 'x': 40, 'y': 65, 'pins': 2, 'color': 'purple'},
        {'name': 'FUSE_OR_JUMPER', 'x': 55, 'y': 65, 'pins': 2, 'color': 'purple'},
    ]
    
    for conn in connectors:
        x, y = conn['x'], conn['y']
        pitch = 2.54
        n_pins = conn['pins']
        
        for i in range(n_pins):
            if n_pins <= 4:
                pin_y = y - (n_pins - 1) * pitch / 2 + i * pitch
                pin_x = x
            else:
                pin_y = y - (n_pins - 1) * pitch / 2 + i * pitch
                pin_x = x
            
            circle = Circle((pin_x, pin_y), 0.35, edgecolor=conn['color'], 
                           facecolor='white', linewidth=1.5)
            ax.add_patch(circle)
        
        ax.text(x + 8, y, conn['name'], ha='left', fontsize=8, 
               weight='bold', color=conn['color'])
    
    testpoints = [
        (10, 10), (25, 10), (40, 10), (55, 10), (70, 10), (85, 10),
        (15, 72), (30, 72), (45, 72), (60, 72)
    ]
    
    for x, y in testpoints:
        circle = Circle((x, y), 0.3, edgecolor='darkred', 
                       facecolor='yellow', linewidth=1)
        ax.add_patch(circle)
    
    resistor_positions = [
        (30, 30), (35, 30), (30, 35), (35, 35), (30, 40), (35, 40),
        (40, 35), (40, 30), (30, 25), (40, 25), (45, 25), (50, 25)
    ]
    
    for x, y in resistor_positions:
        rect = Rectangle((x - 1, y - 0.5), 2, 1, 
                        edgecolor='brown', facecolor='lightyellow', 
                        linewidth=1)
        ax.add_patch(rect)
        for i in range(2):
            pin_x = x - 1.27 + i * 2.54
            circle = Circle((pin_x, y), 0.25, edgecolor='brown', 
                           facecolor='white', linewidth=1)
            ax.add_patch(circle)
    
    gnd_fill_rect = Rectangle((6, 6), 88, 68,
                             edgecolor='darkgreen', facecolor='none',
                             linewidth=2, linestyle=':', alpha=0.5)
    ax.add_patch(gnd_fill_rect)
    ax.text(7, 7, 'GND FILL (B.Cu)', fontsize=9, color='darkgreen', 
           weight='bold')
    
    ax.text(50, 2, 'ESP32-C6 DEVKITC CARRIER', ha='center', fontsize=14, 
           weight='bold')
    ax.text(50, 78, 'ONE-SIDED PTH PCB - HOME ETCHING', ha='center', 
           fontsize=10, style='italic')
    
    ax.set_xlim(0, 100)
    ax.set_ylim(0, 80)
    ax.set_aspect('equal')
    ax.invert_yaxis()
    ax.grid(True, alpha=0.2, linestyle='--')
    ax.set_xlabel('X (mm)', fontsize=12)
    ax.set_ylabel('Y (mm)', fontsize=12)
    ax.set_title('ESP32-C6 DevKitC Carrier Board - PCB Layout', fontsize=14, weight='bold')
    
    legend_elements = [
        patches.Patch(edgecolor='red', facecolor='white', label='ESP32-C6 Headers'),
        patches.Patch(edgecolor='blue', facecolor='white', label='Sensor Connectors'),
        patches.Patch(edgecolor='green', facecolor='white', label='Power Modules'),
        patches.Patch(edgecolor='orange', facecolor='white', label='Battery'),
        patches.Patch(edgecolor='yellow', facecolor='yellow', label='Test Points'),
        patches.Patch(edgecolor='brown', facecolor='lightyellow', label='Resistors/Capacitors'),
        patches.Patch(edgecolor='darkgreen', facecolor='none', linestyle=':', label='GND Fill'),
    ]
    ax.legend(handles=legend_elements, loc='upper right', fontsize=9)
    
    plt.tight_layout()
    
    if output_image:
        plt.savefig(output_image, dpi=300, bbox_inches='tight')
        print(f"PCB visualization saved to '{output_image}'")
    
    plt.show()
    print("PCB visualization complete!")

if __name__ == "__main__":
    pcb_file = "ESP32C6_Carrier_Board.kicad_pcb"
    
    if not os.path.exists(pcb_file):
        print(f"PCB file '{pcb_file}' not found. Running generator first...")
        os.system(f"{sys.executable} esp32c6_carrier_pcb_generator.py")
    
    visualize_pcb(pcb_file, output_image="ESP32C6_Carrier_Board_Layout.png")
