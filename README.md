# Mangrove

Client Git nativo per **Windows 11** e **macOS**. Il codice è condiviso tra le due piattaforme
(`src/core`: lettura del repository, grafo, operazioni Git con undo/redo, diff e patch parziali,
impostazioni, testi e domande dell'interfaccia); solo l'interfaccia è nativa di ciascun sistema:
Win32 + Direct2D con l'aspetto delle app di Windows 11, AppKit su macOS.

Apre cartelle Git e mostra la cronologia con il grafo dei commit, i branch locali e remoti, i tag,
gli stash e le modifiche non ancora committate. Esegue Fetch, Pull, Push, nuovo branch e Stash;
cliccando un file si apre il suo diff, da cui si possono mettere in stage, togliere dallo stage o
scartare singoli hunk e singole righe. Su Windows usa i colori di sistema (modalità chiara o scura e
colore principale) e offre anche il tema originale blu notte e oro (*tema Mangrove*); su macOS
segue l'aspetto chiaro o scuro e il colore di accento del sistema.

![temi](https://img.shields.io/badge/temi-Windows%20%2B%20Mangrove-E3B341)
![licenza](https://img.shields.io/badge/licenza-PolyForm%20Noncommercial%201.0.0-1B2E5E)

## Tecnologia

- **C++20 nativo**, nessun framework UI multipiattaforma: un core condiviso (`src/core`, solo la
  libreria standard più un piccolo livello per i servizi di Windows e di macOS/POSIX) e due
  interfacce native che lo usano direttamente.
- **Windows**: Win32 + **Direct2D/DirectWrite** su una swap chain **DirectComposition** con alpha
  premoltiplicato. Eseguibile singolo da ~1,1 MB, CRT statica.
- **Windows 11**: backdrop **Mica Alt** (`DWMWA_SYSTEMBACKDROP_TYPE`), barra del titolo chiara o
  scura secondo il tema, angoli arrotondati, pulsanti caption nativi DWM con **Snap Layouts**, menu
  contestuali nativi nello stesso tema, font *Segoe UI Variable* e *Segoe Fluent Icons*, DPI
  per-monitor v2.
- **macOS**: **AppKit** in Objective-C++ (ARC): `NSSplitViewController` con barra laterale e
  inspector, toolbar unificata, schede di finestra native, `NSTableView`/`NSOutlineView` con celle
  disegnate, simboli SF, fogli di avviso di sistema, **FSEvents** per l'aggiornamento automatico.
  App universale (Apple silicon e Intel) per macOS 12 o successivo.
- **Dati**: git (`git.exe` su Windows; su macOS quello di Homebrew o degli Xcode Command Line
  Tools), l'implementazione più veloce disponibile, con formati separati da NUL,
  letti in streaming su thread di lavoro: log, ref, stash e status in parallelo. Ordinamento
  topologico per data (come `git log --date-order`) e layout delle corsie in C++.

Prestazioni misurate sul repository di Git (85.596 commit, 1.022 ref): cronologia pronta in
~0,8 s (di cui ~0,7 s di `git log`), layout del grafo 35 ms, ~2 ms per frame durante lo
scorrimento.

## Interfaccia Windows

- **Barra del titolo**: una scheda per repository (come Esplora file), ripristinate alla
  riapertura; pagina iniziale con i repository recenti, apertura da dialogo (`Ctrl+O`),
  trascinamento di una cartella o riga di comando.
- **Barra dei comandi**: percorso *repository › branch* (ognuno apre un menu: repository recenti,
  cambio di branch), poi **Fetch**, **Pull** e **Push** con il numero di commit da ricevere o da
  inviare, **New branch**, **Stash** e **⋯** (Undo/Redo, Pop dello stash, terminale, modalità di
  pull, aggiorna, mostra in Esplora file). Durante un'operazione mostra il progresso di git e una
  barra di avanzamento.
- **Pannello di navigazione**: *Changes* (con il numero di file modificati) e *History*; le sezioni
  **Branches**, **Tags**, **Stashes**, **Remotes** e **Submodules** si espandono sul posto, con
  cartelle e ahead/behind; sotto, i **branch recenti** (quello attivo e quelli con i commit più
  recenti). *Settings* in fondo apre la [finestra delle impostazioni](#impostazioni). Con `Ctrl+K`,
  o quando la finestra è stretta, il pannello mostra solo le icone e ogni sezione si apre come menu.
- **History**: titolo con branch, numero di commit e ora dell'ultimo fetch; ricerca dei commit
  (`Ctrl+F`) per messaggio, autore, SHA o nome di branch/tag, con i risultati evidenziati e
  `Invio`/`F3` per passare al successivo; filtro **All branches / Current branch**.
- **Righe a due livelli** (titolo; autore · tempo relativo · SHA) e la colonna **Changes** con le
  righe aggiunte e rimosse da ogni commit, caricate in background solo per le righe visibili.
- **Grafo** a linee sottili con curve morbide: punti pieni per i commit, anelli per i merge, un
  anello tratteggiato per le modifiche non committate, un quadrato per gli stash.
- **Colonna Branch / tag**: per ogni commit una chip con icone locale (portatile), remoto (nuvola)
  o tag, e `+N` quando sullo stesso commit ci sono altri ref; passando sopra la chip compare
  l'elenco di tutti, con lo stato di ciascuno (*checked out*, *here and on origin*, *local only*,
  *1 to push, 2 to pull*, *on origin only*, *tag*). Doppio clic su una voce per il checkout, clic
  destro per il suo menu. Le colonne si ridimensionano trascinando i bordi delle intestazioni.
- **Dettagli**: SHA con copia, menu del commit, titolo e descrizione, autore con data e parent
  cliccabili, azioni **Check out** (il branch su quel commit, altrimenti il commit in detached HEAD),
  **New branch here**, **Revert**; per uno stash **Pop**, **Apply**, **Delete**. Sotto, i file
  modificati con stato e righe +/−, in lista o ad albero.
- Aggiornamento automatico quando i file del repository cambiano.

### Temi

Si sceglie in *Settings › Appearance*; la scelta viene salvata.

- **Windows (impostazione di sistema)**, il predefinito: segue *Personalizzazione > Colori* di
  Windows come le app di Windows 11, cioè la modalità delle app (chiara o scura) e il colore
  principale con le sue sfumature (lette con le API di Windows, `UISettings`). Se le impostazioni
  cambiano mentre Mangrove è aperto, i colori si aggiornano subito.
- **Windows chiaro** / **Windows scuro**: gli stessi colori con la modalità fissata.
- **Mangrove**: il tema originale, blu notte su Mica con l'oro come colore principale.
- Con un **tema a contrasto** di Windows attivo vengono usati i colori di sistema, opachi, qualunque
  sia la scelta.

### Azioni Git

Uguali sulle due piattaforme (la logica è nel core); i tasti indicati sono quelli di Windows, quelli
di macOS sono [più sotto](#macos). Ogni azione gira su un thread separato: il pulsante mostra uno spinner, la barra dei comandi il
progresso di git in tempo reale, l'esito arriva come notifica e il repository si ricarica.

- **Fetch** di tutti i remoti (i branch eliminati sul remoto vengono rimossi).
- **Pull**: *fast-forward if possible* (merge), *fast-forward only* o *rebase*, scelti dal menu ⋯
  o in *Settings › General*; l'ultima scelta diventa quella del pulsante. Le modifiche locali vengono messe da parte e
  riapplicate in automatico (`--autostash`).
- **Push** del branch corrente; se non ha upstream lo crea su `origin`. Se il remoto è avanti
  compare un dialogo: *Pull* oppure *Force push* con `--force-with-lease`, che si ferma se sul
  remoto ci sono commit non ancora scaricati.
- **New branch** (`Ctrl+B`): campo inline nella colonna Branch / tag del commit selezionato, con
  validazione del nome; il nuovo branch viene creato lì e attivato.
- **Stash** (incluse le modifiche non tracciate) e **Pop**; dal pannello di navigazione o dai
  dettagli anche *Apply* e *Delete* (con conferma).
- **Checkout**: doppio clic su un branch (pannello, chip o elenco della chip) o *Check out* nei
  dettagli; un branch remoto crea il branch locale che lo traccia. Il menu del branch nella barra
  dei comandi cambia branch.
- **Revert** (con conferma): crea un commit che annulla quello selezionato; per un merge annulla le
  modifiche portate dal branch unito (`-m 1`). Non disponibile con modifiche in stage.
- **Undo / Redo** (`Ctrl+Z` / `Ctrl+Y`) per checkout, creazione di branch, stash, commit e revert
  fatti in Mangrove. L'undo di un nuovo branch lo elimina solo se punta ancora al commit di
  partenza; quello di un commit o di un revert solo finché non è stato pushato.

I comandi di scrittura passano dal launcher di Git for Windows, quindi hook, SSH e Git Credential
Manager funzionano come nel terminale; i prompt interattivi sono disattivati. Con un account
collegato in *Settings › Integrations*, fetch, pull e push via HTTPS verso il suo server usano il
suo token (vedi [Account](#account-integrations)).

### Commit (Uncommitted changes)

- La prima riga della cronologia riassume le modifiche non committate; **Review and commit** (o
  *Changes* nel pannello) le seleziona e porta il cursore sul messaggio di commit.
- **Stage / Unstage** di un file o di una cartella (vista ad albero) con i pulsanti che compaiono al
  passaggio sulla riga, oppure *Stage all* / *Unstage all* nelle intestazioni. Le liste si
  aggiornano subito, git conferma in background; i nomi con caratteri speciali (`[`, `*`, `?`)
  sono trattati letteralmente.
- **Discard** (cestino sulla riga o menu contestuale), sempre con conferma; un file non tracciato
  viene eliminato dal disco.
- **Box di commit** fisso in basso: *Summary* con il contatore dei 72 caratteri, *Description* su
  più righe (Invio nel summary passa alla descrizione), selezione con mouse e tastiera,
  copia/incolla, `Ctrl+Invio` per confermare.
- **Amend previous commit**: precompila il messaggio dell'ultimo commit; durante un merge il
  messaggio preparato da git (`MERGE_MSG`) viene proposto in automatico.
- **Undo / Redo** anche per commit e amend (`reset --soft`, le modifiche restano in stage), solo
  finché il commit non è stato pushato.
- Repository di un altro account Windows (*dubious ownership*): pulsante "Trust and open" che
  autorizza il percorso solo dentro Mangrove, senza modificare la configurazione Git globale.

### Diff dei file

- Clic su un file (di un commit, *unstaged* o *staged*) per aprirne il diff al posto della
  cronologia; `Esc` o *Close* per tornare indietro. Il file aperto resta evidenziato nella lista.
- Vista **Unified** o **Split** (affiancata, la scelta viene ricordata); i file solo aggiunti o
  solo eliminati restano su una colonna. Numeri di riga vecchi/nuovi, righe aggiunte in verde e
  rimosse in rosso, parole cambiate evidenziate, rinomine mostrate come `vecchio → nuovo`.
- Frecce ▲▼ per saltare all'hunk precedente/successivo; scorrimento orizzontale con `Shift+rotella`,
  rotella orizzontale o `← →`.
- Sulle modifiche non committate: **Stage file / Unstage file** nell'intestazione; su ogni hunk
  **Stage hunk / Unstage hunk** e, per le modifiche non in stage, il cestino **Discard hunk** (con
  conferma). Quando un file passa da una lista all'altra il diff lo segue, e si aggiorna da solo se
  il file cambia su disco.
- **Singole righe**: clic su una riga modificata per selezionarla, trascinamento o `Shift+clic` per
  un intervallo, `Ctrl+clic` per aggiungerne o toglierne una. La barra che compare in basso offre
  *Stage lines* / *Unstage lines* e *Discard* (con conferma); `Esc` annulla la selezione, che resta
  valida anche se il diff si ricarica. Le righe non selezionate restano come sono: una riga rimossa
  rimane nel file, una aggiunta resta fuori dallo stage.
- Le patch parziali sono costruite da Mangrove e applicate con `git apply` (`--cached` per lo
  stage, `--reverse` per togliere dallo stage o scartare). Le righe di partenza degli hunk vengono
  ricalcolate, così una modifica finisce nel punto giusto anche quando lo stesso testo si ripete nel
  file. Non disponibili per file non tracciati (si mettono in stage interi), conflitti e rinomine.
- Casi particolari: file binari, file nuovi/vuoti, file non tracciati (mostrati come aggiunti),
  conflitti di merge (diff combinato), "nessun a capo a fine file". Le righe lunghissime
  (codice minificato) mostrano 10.000 caratteri attorno alla modifica.
- Prestazioni: solo le righe visibili vengono impaginate e ogni riga resta in cache mentre il diff
  è aperto. Un diff da 200.000 righe (100.000 righe riscritte) si carica in ~120 ms e scorre a
  ~3 ms per frame.

## Interfaccia macOS

L'app per macOS usa i controlli di sistema, con gli stessi dati, le stesse azioni, gli stessi testi
e le stesse conferme della versione Windows:

- **Finestre e schede native**: un repository per scheda (`⌘T`, il `+` della barra delle schede,
  *Window › Merge All Windows*), riaperte all'avvio; pagina iniziale con i repository recenti e
  trascinamento di una cartella; *File › Open Recent*; una cartella trascinata sull'icona nel Dock
  si apre in Mangrove.
- **Toolbar** unificata: barra laterale, **Fetch**, **Pull** e **Push** (da macOS 26 con il numero
  di commit da ricevere o da inviare), **New Branch**, **Stash**, **Pop**, un indicatore di
  attività, la ricerca dei commit e il menu ⋯ (Undo/Redo, terminale, modalità di pull, aggiorna,
  Finder). Il sottotitolo della finestra mostra branch, numero di commit e ultimo fetch, e durante
  un'operazione il progresso di git.
- **Barra laterale** (source list): *Changes* con il numero di file, *History*, poi
  Branches, Remotes, Tags, Stashes e Submodules, con cartelle e ahead/behind; doppio clic per il
  checkout, menu contestuali.
- **Cronologia** in una tabella con colonne ridimensionabili: chip di branch e tag con le icone
  locale / remoto / tag e `+N` (l'elenco completo con lo stato di ciascuno nel tooltip), grafo,
  righe a due livelli, righe +/− caricate per le righe visibili; filtro *All Branches / Current
  Branch* e risultati della ricerca.
- **Dettagli** nell'inspector a destra (si può nascondere): commit con autore, parent cliccabili,
  *Check Out*, *New Branch…*, *Revert…* (per uno stash *Pop*, *Apply*, *Delete…*) e i file
  modificati, in lista o ad albero; per le modifiche non committate le liste *Unstaged* e *Staged*
  con Stage / Unstage (cestino per scartare al passaggio del mouse) e il messaggio di commit con
  *Amend previous commit*.
- **Diff** al posto della cronologia, unificato o affiancato, con stage, unstage e discard di hunk e
  di singole righe (clic, `⇧`-clic o trascinamento, `⌘`-clic).
- Conferme con i fogli di avviso di sistema (pulsanti distruttivi in rosso), esiti delle operazioni
  come avvisi in basso nella finestra, [impostazioni](#impostazioni) in *Mangrove › Settings…*.
- Se git manca (un Mac senza Command Line Tools), Mangrove spiega come installarlo.

## Impostazioni

Una finestra a sé, divisa per categorie: su Windows ha il pannello di navigazione e le schede delle
Impostazioni di Windows 11 (*Settings* in fondo al pannello di navigazione, o `Ctrl+,`); su macOS è
la finestra standard con una toolbar di pannelli (*Mangrove › Settings…*, `⌘,`).

- **General**: riapertura all'avvio dei repository che erano aperti, modalità di pull, diff
  unificato o affiancato.
- **Appearance**: il tema; su Windows anche zoom, pannello dei dettagli e navigazione a sole icone.
- **Git**: versione e percorso di git in uso; le cartelle autorizzate con *Trust* (repository di un
  altro account), ognuna rimovibile.
- **Integrations**: gli account di **GitHub** (anche Enterprise Server), **GitLab** (anche
  self-managed) e **Gitea / Forgejo** (Codeberg o un server proprio).

### Account (Integrations)

- Su **github.com** basta **accedere dal browser**: *Sign in with the browser* chiede a GitHub un
  codice monouso e lo mostra; *Copy the code and open GitHub* lo copia e apre la pagina di GitHub
  dove incollarlo e autorizzare Mangrove (permessi `repo`, `workflow`, `read:user`). Intanto
  Mangrove chiede a GitHub ogni pochi secondi se l'autorizzazione è arrivata (OAuth 2.0 *device
  flow*, RFC 8628, senza client secret), riceve il token e lo tratta come uno incollato. *Cancel*,
  o chiudere la finestra, rinuncia; il codice scade dopo 15 minuti. Le organizzazioni che limitano le
  app OAuth devono approvare Mangrove per i loro repository (oppure si usa un token). GitLab resta
  con i token: i suoi token OAuth scadono dopo due ore.
- Altrimenti un account si collega con un **token di accesso personale**: *Create a token* apre nel
  browser la pagina del servizio per crearlo, con nome e permessi già compilati (GitHub: `repo`,
  `workflow`, `read:user`; GitLab: `read_user`, `read_repository`, `write_repository`), poi lo si
  incolla. Il campo mostra solo pallini e non lascia copiare il token.
- Mangrove verifica il token chiedendo al server a chi appartiene (l'endpoint `/user` delle API,
  solo HTTPS, senza seguire redirect) e avvisa quando non basterebbe per il push (un token classico
  di GitHub senza `repo`).
- Il token viene salvato nel **Gestore credenziali di Windows** o nel **portachiavi di macOS**, mai
  nei file di Mangrove: in `settings.ini` restano solo servizio, server e nome utente. Rimuovendo
  l'account si cancella anche il token, che resta valido sul server finché non lo si revoca lì.
- **Fetch, Pull e Push** via HTTPS verso il server di un account usano il suo token: git lo riceve
  per quella sola esecuzione da variabili d'ambiente (`GIT_CONFIG_COUNT`, `GIT_CONFIG_KEY_n`,
  `GIT_CONFIG_VALUE_n`: un `http.<server>/.extraHeader`, git 2.31 o successivo), quindi mai sulla
  riga di comando né nella configurazione di git, e lo invia solo a quel server. I remoti via SSH o
  su altri server funzionano come prima.

#### L'app OAuth di GitHub

L'accesso dal browser passa per l'app OAuth *Mangrove* registrata su GitHub. Il suo Client ID sta in
`kGitHubClientId` (`src/core/accounts.cpp`): non è un segreto, come quelli di GitHub CLI e Git
Credential Manager, e il client secret non serve. Senza Client ID il pulsante non compare. Chi
pubblica una propria versione registra la sua app:

1. *Settings › Developer settings › OAuth Apps › New OAuth App*
   (<https://github.com/settings/applications/new>).
2. *Homepage URL* e *Authorization callback URL*: l'indirizzo del repository (il device flow non
   usa il callback, ma il campo è obbligatorio).
3. Spuntare **Enable Device Flow**; togliere la spunta a **Expire user access tokens** (Mangrove non
   rinnova i token, che altrimenti scadrebbero dopo 8 ore) e lasciare spento *Allow wildcard
   matching*.
4. Mettere il *Client ID* in `kGitHubClientId`. Per provare un'altra app senza ricompilare, la
   variabile d'ambiente `MANGROVE_GITHUB_CLIENT_ID` sostituisce quello compilato.

## Scorciatoie

### Windows

| Tasto | Azione |
| --- | --- |
| `Ctrl+O` / `Ctrl+T` / `Ctrl+W` | apri repository / nuova scheda / chiudi scheda |
| `Ctrl+Tab` | scheda successiva |
| `↑ ↓ PagSu PagGiù Home Fine` | naviga tra i commit (con un diff aperto: lo scorre) |
| `Ctrl+F` | cerca nella cronologia; `Invio` / `Shift+Invio` risultato successivo / precedente, `Esc` svuota |
| `F3` / `Shift+F3` | risultato successivo / precedente della ricerca |
| `Esc` | annulla la selezione di righe, poi chiude il diff |
| `F5` / `Ctrl+R` | ricarica |
| `Ctrl+K` | pannello di navigazione completo / solo icone |
| `Ctrl+J` | mostra/nascondi i dettagli |
| `Ctrl+C` | copia lo SHA del commit selezionato |
| `Ctrl+B` | nuovo branch sul commit selezionato |
| `Ctrl+Z` / `Ctrl+Y` | undo / redo dell'ultima azione |
| `Ctrl+ +/-/0`, `Ctrl+rotella` | zoom |
| `Ctrl+,` | impostazioni (`Ctrl+Tab` cambia categoria, `Esc` chiude) |

### macOS

| Tasto | Azione |
| --- | --- |
| `⌘O` / `⌘T` / `⌘W` | apri repository / nuova scheda / chiudi |
| `⌘F`, `⌘G` / `⇧⌘G` | cerca nella cronologia, risultato successivo / precedente (anche `↩` / `⇧↩` nel campo) |
| `⌘1` / `⌘2` | cronologia / modifiche non committate |
| `⇧⌘F` / `⇧⌘P` / `⌘P` | fetch / pull / push |
| `⇧⌘N` | nuovo branch sul commit selezionato |
| `⌥⌘S` / `⌥⇧⌘S` | stash / pop |
| `⌥⌘Z` / `⌥⇧⌘Z` | undo / redo dell'ultima azione git (`⌘Z` resta l'annulla del testo) |
| `⌘R` | ricarica |
| `⌃⌘S` / `⌥⌘I` | barra laterale / dettagli |
| `⌥⌘B` | solo il branch corrente |
| `⌘C` | copia l'ID del commit selezionato |
| `⌘↩` | commit |
| `Esc` | annulla la selezione di righe, poi chiude il diff |
| `⌘,` | impostazioni |

## Compilare

### Windows

Requisiti: Visual Studio 2022/2026 con il carico di lavoro *Sviluppo di applicazioni desktop
con C++* e Git for Windows. Il Windows SDK viene scaricato automaticamente dai pacchetti NuGet
ufficiali Microsoft nella cartella `deps` (nessun privilegio di amministratore).

```powershell
.\build.ps1          # release -> bin\Mangrove.exe
.\build.ps1 -Run     # compila e avvia
.\build.ps1 -Dbg     # build di debug
```

### macOS

Requisiti: macOS 12 o successivo e Xcode, oppure solo gli Xcode Command Line Tools
(`xcode-select --install`). Nessun progetto Xcode: lo script compila con `clang++` dalla riga di
comando.

```bash
./build-mac.sh            # release universale (Apple silicon + Intel) -> bin/Mangrove.app
./build-mac.sh --run      # compila e apre l'app
./build-mac.sh --native   # solo per il processore di questo Mac (più veloce)
./build-mac.sh --debug    # build di debug -> bin/Mangrove-debug.app
```

L'app è firmata *ad hoc* e non è notarizzata: compilata su un altro Mac (per esempio scaricata dalla
CI), macOS la blocca al primo avvio. Si sblocca una volta da *Impostazioni di Sistema › Privacy e
sicurezza › Apri comunque*, oppure dal Terminale con `xattr -dr com.apple.quarantine Mangrove.app`.
Per distribuirla ad altri va firmata con un certificato Developer ID e notarizzata.

### Icone

`tools\make-icon.ps1` rigenera `src\windows\mangrove.ico` e `src\macos\AppIcon.png` (da cui
`build-mac.sh` produce l'`.icns`).

### Test

- `tools/coretest` (`run.ps1` su Windows, `run.sh` su macOS): test del core condiviso, uguali sulle
  due piattaforme: testo UTF-8/UTF-16/UTF-32, percorsi e righe di comando, alberi del pannello di
  navigazione, testi dell'interfaccia, file delle impostazioni e un repository vero creato nella
  cartella temporanea e letto con git (cronologia, ref, grafo, status, dettagli, diff, righe +/−).
- `tools/patchtest` (`run.ps1` / `run.sh`): test di regressione delle patch parziali. Crea un
  repository usa e getta nella cartella temporanea, mette in stage, toglie dallo stage e scarta ogni
  hunk, ogni riga e centinaia di selezioni casuali (con file senza a capo finale, CRLF, file
  eliminati o nuovi, blocchi ripetuti) e controlla con `git diff` che si sia spostato esattamente
  ciò che era selezionato, alle righe giuste.
- `tools/uitest` (`mac-snapshots.sh`, `win-snapshots.ps1`): test di fumo dell'interfaccia. Apre un
  repository usa e getta nell'app, che salva le immagini delle sue finestre (cronologia, diff,
  pagine delle impostazioni; su Windows anche nei temi scuro e Mangrove) e si chiude; fallisce se
  l'app si blocca o non le salva. Porta le finestre in primo piano: pensato per le macchine di CI.
- **GitHub Actions** (`.github/workflows/build.yml`): a ogni push su `main` compila l'app su macOS e
  su Windows, esegue i test su entrambe e pubblica gli artifact *Mangrove-macOS.zip* (l'app, si apre con un doppio clic),
  *Mangrove-Windows* (exe) e le immagini delle finestre (*mac-snapshots*, *windows-snapshots*). Si
  può avviare anche a mano (*Run workflow*).

## Struttura

| Cartella / file | Contenuto |
| --- | --- |
| `src/core/` | **codice condiviso**, C++20 portabile |
| `src/core/git.cpp` | esecuzione di git, parser in streaming, ordinamento, dettagli, righe +/−, status, diff e patch parziali |
| `src/core/graph.cpp` | assegnazione delle corsie del grafo |
| `src/core/session.cpp` | `RepoSession`: un repository aperto. Caricamento e aggiornamento automatico, selezione e dettagli, ricerca, righe +/−, operazioni git in coda con undo/redo, diff con la selezione di righe; i risultati tornano al thread dell'interfaccia come eventi |
| `src/core/present.cpp` | testi e domande di conferma mostrati da entrambe le interfacce |
| `src/core/navtree.cpp` | alberi di branch, remoti, tag e file; stato di un ref rispetto al remoto |
| `src/core/settings.cpp` | impostazioni condivise (`settings.ini`) |
| `src/core/accounts.cpp` | account di GitHub, GitLab e Gitea / Forgejo: verifica dei token sul server, token nel credential store di sistema, autenticazione di git via HTTPS |
| `src/core/text.cpp` | testo UTF-8 / wide, percorsi, date, righe di comando |
| `src/core/platform_win.cpp`, `platform_posix.cpp` | servizi del sistema: processi, file, git, orologio, monitoraggio dei file (`ReadDirectoryChangesW` / FSEvents), HTTPS (WinHTTP / libcurl), credenziali (Gestore credenziali / portachiavi) |
| `src/windows/` | **interfaccia Windows**: `app.cpp` (finestra, input, pannello di navigazione, impostazioni), `views.cpp` (disegno delle viste, immediate mode con zone di hit-test), `diffview.cpp`, `actions.cpp` (eventi delle sessioni, dialoghi, campi di testo), `settingswindow.cpp` (finestra delle impostazioni), `gfx.cpp` (Direct2D/DirectComposition), `theme.cpp`, `snapshots.cpp` (immagini per il test dell'interfaccia) |
| `src/macos/` | **interfaccia macOS**, Objective-C++: `App.mm` (applicazione, menu, schede ripristinate), `Preferences.mm` (finestra delle impostazioni), `RepoWindow.mm` (finestra, toolbar, eventi delle sessioni, conferme), `Sidebar.mm`, `History.mm`, `Details.mm`, `DiffView.mm`, `Bridge.mm` (testo, colori, simboli) |
| `tools/` | icone, test del core, delle patch e dell'interfaccia |

Le impostazioni sono salvate in `%APPDATA%\Mangrove\settings.ini` su Windows e in
`~/Library/Application Support/Mangrove/settings.ini` su macOS; la variabile d'ambiente
`MANGROVE_SETTINGS` può indicare un file diverso (profili separati, test). Mangrove prima si chiamava
DreamTree: al primo avvio riprende le impostazioni salvate da DreamTree (schede, repository recenti,
tema). Per i test UI,
`MANGROVE_DEBUG=1` mostra in basso a sinistra zona sotto il puntatore, focus, numero di notifiche,
tempi e, con un diff aperto, righe e layout in cache, e lascia che il puntatore sia guidato da
messaggi inviati alla finestra; `MANGROVE_ACCENT=RRGGBB` sostituisce il colore principale di
Windows e `MANGROVE_CONTRAST=1` simula un tema a contrasto, senza toccare le impostazioni di
sistema; `MANGROVE_SNAPSHOT=<cartella>` fa salvare all'app le immagini delle sue finestre in quella
cartella, poi la chiude (test dell'interfaccia); `MANGROVE_GITHUB_CLIENT_ID` usa un'altra app OAuth
di GitHub per l'accesso dal browser.

## Licenza

Mangrove è distribuito con la [PolyForm Noncommercial License 1.0.0](LICENSE.md): si può usare,
studiare, modificare e condividere liberamente per scopi **non commerciali** (uso personale, studio,
ricerca, hobby, enti non profit, scuole e università, enti pubblici), conservando l'avviso di
copyright. L'uso commerciale non è permesso: per una licenza commerciale scrivi all'autore,
[@Legoraccio](https://github.com/Legoraccio).

Copyright (c) 2026 Roberto Di Miceli.
