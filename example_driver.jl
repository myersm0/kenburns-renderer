using Random

dir = ARGS[1]
paths = sort(filter(f -> endswith(lowercase(f), ".jpg"), readdir(dir, join=true)))
isempty(paths) && error("no jpg files found in $dir")

function random_keyframe(path)
	sx = 0.4 + 0.2 * rand()
	sy = 0.4 + 0.2 * rand()
	sz = 0.5 + 0.2 * rand()
	ex = sx + (rand() - 0.5) * 0.15
	ey = sy + (rand() - 0.5) * 0.15
	ez = sz + (rand() - 0.5) * 0.1
	ex = clamp(ex, 0.2, 0.8)
	ey = clamp(ey, 0.2, 0.8)
	ez = clamp(ez, 0.5, 1.5)
	return (path, sx, sy, sz, ex, ey, ez)
end

function drain_until(process, prefix; timeout=10.0, quit_flag=Ref(false))
	deadline = time() + timeout
	while time() < deadline && !quit_flag[]
		# non-blocking check for data from C++ process
		if bytesavailable(process) > 0 || !isopen(process)
			line = readline(process)
			if startswith(line, "key ")
				code = parse(Int, split(line)[2])
				if code == 27
					quit_flag[] = true
					return "quit"
				end
			end
			if startswith(line, prefix)
				return line
			end
		else
			sleep(0.05)
		end
	end
	return ""
end

function wait_hold(seconds; quit_flag=Ref(false), process=nothing)
	deadline = time() + seconds
	while time() < deadline && !quit_flag[]
		if process !== nothing && bytesavailable(process) > 0
			line = readline(process)
			if startswith(line, "key ")
				code = parse(Int, split(line)[2])
				if code == 27
					quit_flag[] = true
					return
				end
			end
		end
		sleep(0.05)
	end
end

process = open(`./slideshow --interactive --width 1920 --height 1080`, "r+")
readline(process)

quit_flag = Ref(false)

@async while !quit_flag[]
	if bytesavailable(stdin) > 0
		line = readline(stdin)
		if strip(line) == "q"
			quit_flag[] = true
		end
	else
		sleep(0.05)
	end
end

index = 1
path, sx, sy, sz, ex, ey, ez = random_keyframe(paths[index])
println(process, "image $path $sx $sy $sz $ex $ey $ez")
readline(process)

while !quit_flag[]
	global index = mod1(index + 1, length(paths))
	global path, sx, sy, sz, ex, ey, ez = random_keyframe(paths[index])
	println(process, "image $path $sx $sy $sz $ex $ey $ez")
	readline(process)
	wait_hold(4.0; quit_flag, process)
	if quit_flag[] break end
	println(process, "next")
	result = drain_until(process, "done"; quit_flag)
	if result == "quit" break end
end

println(process, "quit")
close(process)

