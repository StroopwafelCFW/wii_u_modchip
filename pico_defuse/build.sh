mkdir -p build &&
cd build && 
cmake .. && 
make &&
sudo picotool load -f pico_defuse.uf2 && 
echo "Done!"