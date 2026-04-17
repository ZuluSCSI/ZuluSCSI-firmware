#ifdef PLATFORM_AS400
	uint8_t as400_serial[8];
	as400_get_serial_8(scsiDev.target->targetId & S2S_CFG_TARGET_ID_BITS, as400_serial);
#endif

#ifdef PLATFORM_AS400
		else if(config->quirks == S2S_CFG_QUIRKS_AS400 && config->deviceType == S2S_CFG_FIXED)
		{
			char serial_string[sizeof(as400_serial)+1] = {0};
			memcpy(serial_string, as400_serial, sizeof(as400_serial));
			memcpy(scsiDev.data, AS400VendorInquiry, AS400VendorInquiryLen);
			// Rewrite serial
			memcpy(&scsiDev.data[38], as400_serial, sizeof(as400_serial));
			scsiDev.dataLen = AS400VendorInquiryLen;
			scsiDev.phase = DATA_IN;
		}
#endif


#ifdef PLATFORM_AS400
	else if (config->quirks == S2S_CFG_QUIRKS_AS400 && config->deviceType == S2S_CFG_FIXED)
	{
		bool found_page_code = false;
		for (uint8_t i = 0; i < AS400VitalPagesLen; i++)
		{
			if (AS400VitalPages[i][2] == pageCode)
			{
				uint8_t length = AS400VitalPages[i][0];
				memcpy(scsiDev.data, AS400VitalPages[i]+1, length);
				scsiDev.dataLen = length;
				found_page_code = true;
				switch(pageCode)
				{
					case 0x80: // serial number
						memcpy(&scsiDev.data[12], as400_serial, sizeof(as400_serial));
						break;
					case 0x82: // has partial 5 letter serial number in it
						memcpy(&scsiDev.data[16], as400_serial, 6);
						break;
					case 0x83:
						memcpy(&scsiDev.data[34], as400_serial, sizeof(as400_serial));
						break;
					case 0xD1:
						memcpy(&scsiDev.data[70], as400_serial, sizeof(as400_serial));
						break;
					default: // do nothing
						break;
				}
				break;
			}
		}
		if (found_page_code)
		{
			scsiDev.phase = DATA_IN;
		}
		else
		{
			scsiDev.status = CHECK_CONDITION;
			scsiDev.target->sense.code = ILLEGAL_REQUEST;
			scsiDev.target->sense.asc = INVALID_FIELD_IN_CDB;
			scsiDev.phase = STATUS;		
		}
	}
#endif



                int x,y;
                uint8_t *z = buf;
                if (len %  bytesPerSector  != 0)
                {
                    logmsg("SKIP: transfer length ",(int) len ," not multiple of ", (int) bytesPerSector, " bytes per sector!");
                }
                x = len / bytesPerSector;

                while (x)
                {
                    y = skip_next(x);                    
                    if (y < 0) {//skips                        
                        img.file.seek(img.file.position() + (abs(y) * bytesPerSector));                                                
                        continue;
                    }
                    else if (y > 0) {                        
                        img.file.write(z, y * bytesPerSector);
                        x -= y; //reduce remaing
                        z += (y * bytesPerSector); //advance location in buffer
                    }
                    else {//we must be done.
                        break;




 if (g_disk_transfer.skip_command == 0xE8)
    {
        int x, y;
        uint8_t *z = buffer;

        uint32_t bytesPerSector = scsiDev.target->liveCfg.bytesPerSector;
        if(count % bytesPerSector != 0)
        {
            logmsg("SKIP: transfer count ",(int) count ," not multiple of ", (int) bytesPerSector, " bytes per sector!");
        }
        x = count / bytesPerSector;

        while(x)
        {
            y = skip_next(x);
            if (y < 0) {//skips
                img.file.seek(img.file.position() + (abs(y) * bytesPerSector));
                continue;
            }
            else if (y > 0)
            {
                img.file.read(z, y * bytesPerSector);
                x -= y; //reduce remaing
                z += (y * bytesPerSector); //advance location in buffer
            }
            else 
            {
                //we must be done.
                break;
            }
        }
    }