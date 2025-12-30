sudo rmmod osfs.ko
sudo dmesg
sudo insmod osfs.ko
sudo dmesg
sudo mount -t osfs none mnt/
sudo dmesg
