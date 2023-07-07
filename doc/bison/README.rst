.. raw:: html

   <a href="https://github.com/TuGraz-ITI/zephyr/tree/main">
     <p align="center">
       <picture>
         <source media="(prefers-color-scheme: dark)" srcset="assets/logo_white.svg">
         <source media="(prefers-color-scheme: light)" srcset="assets/logo_black.svg">
         <img width="20%" src="assets/bison.svg">
       </picture>
     </p>
   </a>

BISON: Attacking Bluetooth's Broadcast Isochronous Streams
***************
This repository contains the source code for the BISON attack. This attack targets Bluetooth's broadcast isochronous streams (BISes), exploiting the plaintext metadata as well as the vague specification of the Broadcast_Code exchange. Further information can be found in the corresponding paper by Theo Gasteiger, Carlo Alberto Boano, and Kay Römer in Proc. of the 20th EWSN Conf. 2023. 

Video Demonstration
***************

.. image:: https://img.youtube.com/vi/KWuePWPpFf8/maxresdefault.jpg
    :alt: IMAGE ALT TEXT HERE
    :target: https://www.youtube.com/watch?v=KWuePWPpFf8

Alice and Bob
***************

Alice and Bob are based on the default Zephyr broadcast audio source and broadcast audio sink samples and can be found in the `BISON_ALICE_BOB <https://github.com/TuGraz-ITI/zephyr/tree/BISON_ALICE_BOB>`_ branch.

Mallory
***************

Mallory is based on the default Zephyr isochronous receiver sample but with additional BLE controller changes. Mallory can be found in the `BISON_MALLORY <https://github.com/TuGraz-ITI/zephyr/tree/BISON_MALLORY>`_ branch.

Getting Started
***************

To simplify the demonstration of BISON, precompiled binaries for `Alice <https://github.com/TuGraz-ITI/zephyr/tree/BISON_ALICE_BOB/samples/bluetooth/broadcast_audio_source/bin>`_, `Bob <https://github.com/TuGraz-ITI/zephyr/tree/BISON_ALICE_BOB/samples/bluetooth/broadcast_audio_sink/bin>`_ and `Mallory <https://github.com/TuGraz-ITI/zephyr/tree/BISON_MALLORY/samples/bluetooth/iso_attack/bin>`_ have been added. However one must consider that these binaries require a Nordic Semiconductor nRF5340 Audio development kit for Alice and Bob as well as a nRF52840DK for Mallory.

If compillation is necessary, one can use the custom build scripts labeled ``build.py`` (e.g., `Mallory build.py <https://github.com/TuGraz-ITI/zephyr/blob/BISON_MALLORY/samples/bluetooth/iso_attack/build.py>`_)

Additionally one needs to add a 16 bit little-endian PCM encoded mono audio track with the name ``music.raw`` to the SD card of Alice. One example sound can be found `here <https://github.com/TuGraz-ITI/zephyr/tree/main/doc/bison/music.raw>`_.
