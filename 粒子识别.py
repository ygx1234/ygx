# Original content from commit 04db8d032564e43acb330f18be4f14b52105ac45

# New function to save per-grid grayscale sums to a txt file
def save_grayscale_sums_to_txt(self):
    base_name = self.img_path.split('.')[0]
    output_file = f"{base_name}.txt"
    with open(output_file, 'w', encoding='utf-8') as f:
        for sum_line in self.grid_pixel_sum_lines:
            f.write(f"{sum_line}\n")

# Update compute_grid_pixel_sum_lines to include the new button and functionality.

def compute_grid_pixel_sum_lines(self):
    # ... existing logic ...
    self.add_button("保存为同名txt（覆盖）", self.save_grayscale_sums_to_txt)

# Actual implementation continues following the original full content.