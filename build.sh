mkdir -p ./bld
cmake -B ./bld -S .
cd ./bld && make -j4
