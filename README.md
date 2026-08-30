# ninit
unfinished
### why would i use this?

you dont want to use this

### i do want to use it?

youre suuuper sure?

### yes!

this is what you have to do
1. install it
2. write all your services (check docs) and put them in /etc/ninit/...
3. run `ninitctl init`
4. make a new loader entry with init=/bin/init (could be a symlink to usr bin) (do NOT just ball it on your only one!)
5. reboot and enjoy speed or some failiure becasue you forgot an entry
6. confused? read the source code
