function process_midi(status, data1, data2)

    -- if this is a keyup, then don't do anything, just return
    -- whatever was sent in
    if (data1 == 128) then
        return status, data1, data2;
    end

    -- start the decrescendo when we the high F; we start by decreasing the volume by 1 unit for each note...
    if (data1 == 101) then
        print('start decrescendo...');
        volume = 125             -- set initial volume to 90 (= Loud)
        increment = 1
    end
    
    volume = volume - increment;

    print("volume: " .. volume);

    return status, data1, volume;      
  end

  -- code down here runs when the script is initially loaded...
  -- so we just need to initialize everything
  volume = 90
  increment = 1
  print("example 7 octave decrescendo");