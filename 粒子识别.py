def compute_grid_pixel_sum_lines(img_path):
    # Existing processing logic here...

    # New button to save grayscale sums:
    save_button = tk.Button(window, text="Save Grayscale Sums", command=save_grayscale_sums)
    save_button.pack()

def save_grayscale_sums():
    # Get the grayscale sum lines (this is a placeholder, replace with your actual data)
    sum_lines = ["A1 226", "A2 150"]  # Example data
    base_name = os.path.splitext(os.path.basename(img_path))[0] if img_path else "default"
    
    # Save to .txt file
    with open(f"{base_name}_grayscale_sums.txt", "w") as f:
        for line in sum_lines:
            f.write(line + "\n")
