Please use a machine translator if you do not speak Hungarian!

Bevezető
-

Egy szokásos N-test szimuláció kiegészítve két extrával:

1. A tér lehet wrapped (Pacman stílusú)
2. Tágulhat a tér (kozmikus tágulás szimulációja)

GPU gyorsítással működik, OpenCL-et használ.

Wrapped (Pacman) tér koordináták
-

A wrapped tér azt jelenti, hogy a tér zárt és véges. Azaz nincs széle, úgymond visszafordul magába, mint a Pacman játék (csak most 3D-ben).
Maga a megvalósítás nem bonyolult. Legyen mindegyik koordináta a [0, 1] tartományban! (Azaz a világ mérete egy egység.)
Ekkor két számításra kell figyelni:

1. Mikor odébb mozdul a részecske, akkor a helyét kell wrappolni.
2. Mikor távolságot számítunk, akkor is úgymond át kell wrappolni a távolságot.

Ezeket a számításokat a floor() függvénnyel operálva oldja meg, ami gyors és branchless kódot eredményez (GPU-n is hatékony).

Wrapped (Pacman) tér anizotrópia probléma
-

Van egy súlyos probléma még a wrapped (pacman) világgal. Az, hogy nem izotróp a tér-egységkocka.
A csúcsok iránya felől átlagosan (homogén esetben) nagyobb gravitációs hatás jön, mint a másik irányokból.

Tehát hiába nincs „közepe” és „széle” a wrapped (pacman) világnak, de maga az univerzum mégse izotróp!
Ami fizikailag helytelen.
Ezt úgy lehet lekezelni, hogy minden részecske esetén csak az egység átmérőjű gömbön belüli többi részecske vonzását számoljuk el.
(Azaz fél sugáron belülit.)

Tágulás elszámolása
-

Maga a tér tágulása könnyen programozható lenne pl. így:

1. Megnöveljük a világ méretét és vele a részecskék koordinátáját átskálázzuk.
2. És a sebességeket meg arányosan lecsökkentjük. (Ez fizikai hatása a tágulásnak.)

De ha így járnánk el, akkor wrapped függvényeket kellene elbonyolítani, ami feleslegesen lassítani és bonyolítaná a kódot.
Ezért helyette nem skálázom át a koordinátákat, hanem a gravitációs hatás számításában a számolt távolságokat növelem fel.
Vagyis, hogy még gyorsabb legyen a kód, egyszerűen a G állandót csökkentem le négyzetesen.

Természetese a softening paramétert is arányosan kell csökkenteni az erő számításakor. (Ha nem tudod mi ez, akkor nézz után a szakirodalomban!)

A sebességeket ezért kétszer kell csökkenteni: egyszer a fenti fizikai oka miatt, másrészt mert a tágulással nem növeljük meg a koordinátákat.

Mikor számoljuk el a tágulás sebességre vett hatását?
-

A nehezebb probléma, hogy hol végezzük el a sebesség változtatását. Az N-test számítás lépései a HalfKick, Drift, Accelration számítás, második HalfKick.
(Ha nem tudod miért, akkor nézz után a szakirodalomban!)

Ez azért probléma, mert ha a tágulás hatását (sebességek csökkentése) egy időlépés végén (vagy elején) hajtanánk végre,
akkor az első half-kick + drift másféle koordinátarendszerben (kissé tágult) dolgozna, mint a második half-kick.
Bár ez nagyon minimális hatással lenne a számításokra, de mégse helyes fizikai szempontból.
Ezért a tágulás hatását a drift után a gyorsulás számítás elé raktam be.

Ez azért is jó, mert felmerülhet a kérdés, hogy a gyorsulásokat nem kellene átskálázni a tágulással?
És ha igen, akkor hogyan?
Ezt nem tudom biztosan; de így nem is kell.
Mert úgyis teljesen új gyorsulásokat számolunk miután a tágulás sebesség csökkentő hatását elszámoltuk.

Igaz, hogy magát a scale változót ténylegesen az időlépés végén növelem meg (a második HalfKick után);
de ez nem számít semmit se, mert nem befolyásolja a következő lépés half-kick + drift-jét.
(Azért tettem ide a kódban, mert a scale változtatása globális, nem osztható szét a thread-ek között, ahogy az idő növelése se.

GPU gyorsítás
-

Paraméterek
-

A programban van egy sor paraméter, amiket be kell állítani egy konkrét futás előtt.
A legtöbb paraméter magáért beszél.
Ami lényeges, hogy hogyan állítsuk be a G és DT és EXPANSION_FACTOR paramétereket.
A G és DT önmagában nem jelent semmit, hiszen a szimuláció dimenziómentesített (nincs mértékegység).
Ezért nem az számít, hogy konkrétan mennyi a G, hanem a G és DT együtt számít (és valamennyire az N is).
Ha G kicsi, akkor DT lehet nagyobb és fordítva.
A lényeg az, hogy elég kicsi legyen a DT, hogy egy időlépésben csak kicsit gyorsuljanak és mozogjanak a részecskék.
Valamennyire a SOFTENING is hatással van erre, azt se szabad túl kicsire beállítani, túl nagy meg nem lesz elég realisztikus.

Tapasztalatom szerint pár ezer részecskeszámnál (N), ha a G=1, akkor DT néhányszor 1E-6 legyen.
(Ha G csökken, akkor persze nőhet a DT, de ha nagyon sok részecske van, akkor a gravitációs hatás megnőhet,
ha azok kis helyre koncentrálódnak, ezért nagy N esetén érdemes kisebb DT-t választani.)

A tágulás lineáris, és a EXPANSION_FACTOR adja meg, hogy mennyit tágul egy időegység alatt.
Tehát ha például DT=1E-6 és az EXPANSION_FACTOR=100, akkor egy időlépés (1E-6 időegység) alatt
a tér 1E-4 távolságegységgel növekszik meg (fizikai szimulációs szempontból).

Képek mentése (animáció)
-

Animációt direkt nem tud csinálni a program. Helyette PNG képekbe tudja lementeni az előállított 2D/3D renderelt frame-eket.
A program adott időlépés után (steps paraméter) csinál egy új képet, amit megjelenít az ablakban,
és user választása szerint lementi PNG fájlba is, adott mappába (outdir).

A PNG képekből (frame0000.png, frame0001.png, …) videót valamilyen külső programmal lehet csinálni. Ajánlom az ingyenes mplayer-t (mencoder)!
