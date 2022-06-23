# tcpql
tcpql is a new congestion control algorithm based on q-learning.


## Envs
os : Ubuntu 20 <br>
kernel : 4.14.146.mptcp

#### you can install mptcp easily as follows
download mptcp kernel deb packages
```
wget https://github.com/multipath-tcp/mptcp/releases/download/v0.94.7/linux-headers-4.14.146.mptcp_20190924124242_amd64.deb
wget https://github.com/multipath-tcp/mptcp/releases/download/v0.94.7/linux-image-4.14.146.mptcp_20190924124242_amd64.deb
wget https://github.com/multipath-tcp/mptcp/releases/download/v0.94.7/linux-libc-dev_20190924124242_amd64.deb
wget https://github.com/multipath-tcp/mptcp/releases/download/v0.94.7/linux-mptcp-4.14_v0.94.7_20190924124242_all.deb
```
install all .deb files
```
sudo dpkg -i linux-*.deb
```
set mptcp as the default grub kernel
```
sudo cat /etc/default/grub | sed -e "s/GRUB_DEFAULT=0/GRUB_DEFAULT='Advanced options for Ubuntu>Ubuntu, with Linux 4.14.146.mptcp'/" > tmp_grub
sudo mv tmp_grub /etc/default/grub
sudo update-grub
```

## usage
clone the repo
```
git clone https://github.com/opendev2020/tcp_ql.git
```
build
```
cd tcp_ql
make all
```
insert the module
```
sudo insmod tcpql.ko
```
set tcpql as current congestion control
```
sysctl net.ipv4.tcp_congestion_control=tcpql
```