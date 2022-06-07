for i in {40..79}; do
	echo "Enabling logical HT code $i."
	echo 1 > /sys/devices/system/cpu/cpu${i}/online;
done
