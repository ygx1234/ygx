import tkinter as tk
from tkinter import filedialog

# ... existing code ...

def compute_grid_pixel_sum_lines():
    # existing functionality

    # Add '保存为同名txt' button
    save_button = tk.Button(window, text='保存为同名txt', command=save_as_txt)
    save_button.pack()


def save_as_txt():
    # Get image path, if available
    img_path = get_img_path()  # Assuming you have a method to get this
    if not img_path:
        img_path = filedialog.askopenfilename(title='Select Image')
    
    # Calculate sums (assuming it's being done in compute_grid_pixel_sum_lines)
    sums = calculate_per_grid_sums()  # You need to implement this
    
    # Prepare the content to save in 'A1 226' format
    content = '\n'.join([f'A{i+1} {s}' for i, s in enumerate(sums)])
    
    # Save to text file
    txt_file_path = img_path.rsplit('.', 1)[0] + '.txt'
    with open(txt_file_path, 'w') as f:
        f.write(content)
        
    print(f'Saved grayscale sums to {txt_file_path}')  # A feedback message

# ... rest of your existing code ...