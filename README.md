# ce_cluster_simhub
i bought a gauge cluster from a 2012 ic ce200. it works with an esp32 and can  *in the future* be used in sim games, such as euro truck sim 2, american truck sim, and beamng.drive, among others.

![s-l1600](https://github.com/user-attachments/assets/796f0d6f-4943-4c3b-822a-23363e6e26dd)

# the original thought
at first i wanted to try and wire an older 1990s gauge cluster from an international truck (see picture), but there was no documentation. ![s-l1600adsf](https://github.com/user-attachments/assets/e5132cfb-9e85-425a-85bd-eabc92f4ebfd)

therefore, i settled on something newer (ish). i decided to try and make a '10-'19 ic ce gauge cluster work with my sim games (mainly beamng and ats/ets2). 
# what's needed?
the hardware is pretty easy. i used (and i suggest you use this too) an esp32, seengreat sn65hvd230 can bus transceiver or similar transceiver, a 12 volt power supply, and a 120 ohm resistor (or two if you don't use the seengreat transceiver)
| Transceiver | ESP32 | Power Supply |
| ----------- | ----- | ------------ |
|![61QHGTdeIqL _AC_SL1500_](https://github.com/user-attachments/assets/93856e8e-ac9e-457d-bab2-d92fe4ea836c) | ![716NQlNw7sL _AC_SL1500_](https://github.com/user-attachments/assets/2a953fd4-3bc2-46b4-ae62-fcc72afb7341) | ![71qniwQTBdL _SL1500_](https://github.com/user-attachments/assets/22a031b1-1d4d-4c5a-9f2b-83113dce38b0) |

for specifics:
- elegoo esp32 devkit, on amazon for $17 US
- seengreat sn65hvd230 'CAN HAT' transceiver (just means that the part where the can wires are attatched can come off, ~$9 US)
- 12 volt power supply (i used a 4.5 amp supply, but i wouldn't go higher than 5 amps
- 120 ohm resistors (i went with edgelec, butas long as they measure 120 it should be fine. maybe you could also try a 60 ohm resistor if your transceiver doesn't have a built in 120 ohm resistor jumper?
- instrument panel from an ic ce or international 4300 (known working part numbers are 3868859C91, more to be added in the future) (range in cost, anywhere from $100 US to well in excess of $300 US
- i recommend buying the pigtail connectors from a truck. it makes wiring easy in my opinion. they can range from $10-20 on ebay, or if you want to try the parts warehouses, that have the connectors in stock, go for it! part #s for the connectors are 3548792C1 for the 14 pin power and 2018590C1 for the can bus wires.
- some female to female dupont wires, for the ESP32
- haven't tested it yet, but i'd advise an esp32 breakout board, if your esp32 can fit in it.
- luck?

# WIP - supported clusters:
right now, there isn't a huge amount of known good clusters that will work but:
| '12-'20 | '08-'14 | '11 | '21-'24 | '05-'08 |
| ------- | ------- | --- | ------- | ------- | 
| ![s-l500](https://github.com/user-attachments/assets/cf5c445b-e39e-42ed-acf8-a1431944f4d1) | ![s-l1600asdfsdaf](https://github.com/user-attachments/assets/0b775bd8-0e34-4817-9d9e-261d8e06d9d2) | ![s-l1600qwer](https://github.com/user-attachments/assets/718baa35-2bbc-4257-aedc-39b9a57ec390) | ![s-l1600zxcv](https://github.com/user-attachments/assets/9bc5cb5a-be34-4c96-8b1c-3ee25158d1c2) | ![s-l1600qwerqwer](https://github.com/user-attachments/assets/ff53cb2b-d79f-4c02-94ec-2255d954150c)
| 3868859C9x, 100% supported | 3803476F9x, could work? not yet supported | 3686319c91, similar but different? not yet supported | 3868856F98, before the next gen bus, similar story to '11? not yet supported | oldest 2nd gen cluster, 3581055C9x or similar, not yet supported |

as of now, i'd recommend that beginners start with 3868859C9x, as its the most widely used cluster across all years, and also pretty widely available.
all the pictures for the above gauge clusters were from ebay, and thats where i got my cluster from. if you can find a scrapyard with IC busses in them, pull the cluster from it. ebay tends to set you back anywhere from $100 Us to over $300 US
# supported games
haven't established communication with simhub yet, but simhub and BeamNG will all be supported when this project is finally done.

# how do i set it up?
wiring is pretty easy. on the back of the cluster, there are 3 connectors. if you have one up top, you can ignore it, as it is used for a daylight sensor (optional, can run you ~$15 US on ebay.) the important ones are a 12 pinner on the left, and next to it is a 14 pinner.
<img width="960" height="720" alt="s-l960 - Copy" src="https://github.com/user-attachments/assets/c4d28591-71da-41b8-9ef4-932eea92566f" />

the 12 pinner is the can bus wires, and the 14 pinner is the cluster power.
| 14 pin connector (1500) | 12 pin connector (1505) |
| ----------------------- | ----------------------- |
| 1 - ground | 9 - outdoor temp sensor side A (not on most clusters) |
| 2 - 12 volt constant power | 10 - headlight enable (could wire a relay to enable headlights?) |
| 7 - 12 volt ignition power | 11 - J1939- (CAN LO) |
| 8 - outdoor temp sensor side B (not on most clusters) | 12 - J1939+ (CAN  HI) |

with my attatched code, you can control:
- warning lamps (all but ABS, as it's a fault lamp, and the hydraulic brake lights, service parking brake, brake pressure, and brake fluid)
- most gauges (air gauges, transmission temp, DEF, etc are still a WIP)
- engine oil level warning message (<=80%)
  
